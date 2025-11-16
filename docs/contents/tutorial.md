# Tutorial

The best way to learn a new library is study from examples, let's go through some examples and you'll learn all you want :)

This tutorial assume you have a basic knowledge of `std::execution`. If you are not familiar with it, we recommend the following resources:

1. [P2300](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html) proposal
2. [stdexec's doc](https://github.com/NVIDIA/stdexec?tab=readme-ov-file#resources) 

C++26 is not finalized now. Currently we're based on the early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), so you'll see `stdexec`, `exec` namespaces
instead of `std::execution` in the following examples.

## Basic case - turn your class into an actor

First let's go through a basic example - create your first actor and call it.
<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

// 0. Assume you have a class, you want to turn it into an actor.
struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};


int main() {
  // 1. First, create a ex_actor::ActorRegistry.
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/2);

  // 2. Use the registry to create an actor.
  ex_actor::ActorRef actor = registry.CreateActor<YourClass>();
  
  /*
  2. Everything is setup, you can call the actor's method now using `actor_ref.Send`.
  
  This method returns a standard `std::execution::task`, compatible
  with everything in the `std::execution` ecosystem.

  This method requires your args can be serialized by reflect-cpp, if you met compile
  errors like "Unsupported type", refer https://rfl.getml.com/concepts/custom_classes/
  to add a serializer for it.
  
  Or if you can confirm it's a local actor, use SendLocal() instead. See below.
  */
  auto task = actor.Send<&YourClass::Add>(1);

  /*
  2.1 For local actors, you can try `SendLocal`, which doesn't require the args to be serializable.
  */
  auto sender = actor.SendLocal<&YourClass::Add>(1);

  /*
  3. To execute the task and blocking wait for the result, use `sync_wait`.
  Note that the task is not copyable, so you need to use `std::move`.
  */
  auto [res] = stdexec::sync_wait(std::move(task)).value();
  assert(res == 1);
}
```
<!-- doc test end -->

## Wrap the result using coroutine

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};

exec::task<int> Coroutine() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/2);
  ex_actor::ActorRef actor = registry.CreateActor<YourClass>();
  // this is non-blocking, the thread will be able to process other actors while waiting for the result.
  auto res = co_await actor.Send<&YourClass::Add>(1);
  assert(res == 1);
  co_return res + 2;
}

int main() {
  auto [res2] = stdexec::sync_wait(Coroutine()).value();
  assert(res2 == 3);
}
```
<!-- doc test end -->

## Chain actors - send message from one actor to another

This examples shows how to call an actor's method from another actor.

The main thread calls `Proxy`, then `Proxy` calls `Counter`.


<!-- doc test start -->
```cpp
#include <cassert>
#include <iostream>
#include "ex_actor/api.h"

class Counter {
 public:
  void Add(int x) { count_ += x; }
  int GetValue() const { return count_; }

 private:
  int count_ = 0;
};

class Proxy {
 public:
  explicit Proxy(ex_actor::ActorRef<Counter> actor_ref) : actor_ref_(actor_ref) {}
  
  exec::task<int> GetValue() {
    co_return co_await actor_ref_.template Send<&Counter::GetValue>();
  }

 private:
  ex_actor::ActorRef<Counter> actor_ref_;
};

int main() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/2);
  ex_actor::ActorRef counter = registry.CreateActor<Counter>();

  // 1. increase the counter 100 times
  exec::async_scope scope;
  for (int i = 0; i < 100; ++i) {
    scope.spawn(counter.Send<&Counter::Add>(1));
  }
  stdexec::sync_wait(scope.on_empty());

  // 2. create a proxy actor, who has a reference to the counter actor
  ex_actor::ActorRef proxy = registry.CreateActor<Proxy>(counter);

  // 3. call through the proxy actor
  auto [res2] = stdexec::sync_wait(proxy.Send<&Proxy::GetValue>()).value();
  assert(res2 == 100);
}
```
<!-- doc test end -->

## [Optional Read] Wrap the result using sender adapter

We recommend you to use coroutine to wrap the result, which is easier and more readable.

But if you insist on using sender adapter for some reason, be cautious that you'll lose the scheduler affinity `std::execution::task` provided,
the `ex::then` callback will run on the actor's thread, **do not capture local variable's reference, or `this` pointer of an actor instance in the callback**.

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};

int main() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/2);
  ex_actor::ActorRef actor = registry.CreateActor<YourClass>();

  // Sender adapter style
  int local_var = 1;
  auto task1 = actor.Send<&YourClass::Add>(1) | stdexec::then([&local_var](int value) {
    // this line will be executed on the actor's thread.
    // local_var will have data race
    local_var++;
    return value + 1;
  });

  // data race
  local_var++;

  auto [res1] = stdexec::sync_wait(std::move(task1)).value();
  assert(res1 == 2);
}
```
<!-- doc test end -->

In the above example, the ex::then callback runs on the actor's thread. If you capture local variables in the ex::then callback, it will cause data race.

A more dangerous example is capturing `this` in actor's method.

<!-- doc test start -->
```cpp
#include <cassert>
#include <iostream>
#include "ex_actor/api.h"

struct DummyActor {
  void Ping() {}
};

class Proxy {
 public:
  explicit Proxy(ex_actor::ActorRef<DummyActor> actor_ref) : actor_ref_(actor_ref) {}

  stdexec::sender auto SomeMethod() {
    // DO NOT capture `this` in the callback, it will cause data race
    return actor_ref_.template Send<&DummyActor::Ping>() | stdexec::then([this]() {
             // this line will be executed on the DummyActor's thread.
             // the next line will have data race
             actor_member_var++;
           });
  }

  void AnotherMethod() {
    // the other side of the data race
    actor_member_var++;
  }

 private:
  int actor_member_var = 0;
  ex_actor::ActorRef<DummyActor> actor_ref_;
};

int main() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/2);
  ex_actor::ActorRef dummy_actor = registry.CreateActor<DummyActor>();

  // 2. create a proxy actor, who has a reference to the dummy actor
  ex_actor::ActorRef proxy = registry.CreateActor<Proxy>(dummy_actor);

  // 3. call through the proxy actor
  exec::async_scope scope;
  scope.spawn(proxy.Send<&Proxy::SomeMethod>());
  scope.spawn(proxy.Send<&Proxy::AnotherMethod>());
  stdexec::sync_wait(scope.on_empty());
}
```
<!-- doc test end -->

### Understanding the scheduler affinity of std::execution::task

To understand why the callback of ex::then runs on the target actor's thread, while in coroutine it runs on the caller's thread, you need to know the scheduler switching mechanism in `std::execution`.

In `std::execution`, scheduler's switch should be explicit - by calling `continue_on` explicitly.

An actor itself is a scheduler (not the scheduler passed to the `ActorRegistry` constructor, but **actor itself**), when you call its method, you schedule a task on it.
So all the callbacks will run on the actor's thread.

But in a coroutine, the code **looks like** they are executing in the same thread.
So in order not to confuse the user, make coroutine easy to use, `std::execution::task` has **scheduler affinity** - it will keep the scheduler the same across the entire coroutine.
In other words, after any `co_await` in the coroutine, `std::execution::task` will help you to switch back to the coroutine's scheduler.
(See [`std::execution::task`'s proposal](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3552r3.html) for more details).
