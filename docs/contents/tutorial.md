# Tutorial

The best way to learn a new library is to learn from examples, let's go through some examples and you'll learn all you want :)

This framework is based on `std::execution`, but **it's fine if you are not familiar with it, the example is easy enough to understand**.

std::execution is essentially an unified interface for schedulers and async tasks. It standardizes the interfaces so that everyone
conforming the standard can use others' work seamlessly. Check [P2300 proposal](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html) and [stdexec's doc](https://github.com/NVIDIA/stdexec?tab=readme-ov-file#resources) if you want to learn more.

Note: C++26 is not finalized now. Currently we're based on the early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), so you'll see `stdexec`, `exec` namespaces
instead of `std::execution` in the following examples.

## Basic case - turn your class into an actor

First let's go through a basic example - create your first actor and call it.

Nearly all of our APIs are async, so let's put everything in a coroutine, then we can easily use `co_await` to wait for the result non-blockingly.
Here we use [`std::execution::task`](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3552r3.html), which is the standard coroutine type in `std::execution`.

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

// 0. Assume you have a class, you want to turn it into an actor.
struct Counter {
  int Add(int x) { return count += x; }
  int count = 0;
};

/*
1. First, create a ex_actor::ActorRegistry, usually as a global variable.
   All methods of ActorRegistry are thread-safe.
*/
ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);

exec::task<void> MainCoroutine() {

  // 2. Use the registry to create an actor.
  ex_actor::ActorRef actor = co_await registry.CreateActor<Counter>();
  
  /*
  3. Everything is setup, you can call the actor's method now using `actor_ref.Send`.
  This method returns a standard `std::execution::task`, compatible with everything
  in the `std::execution` ecosystem. If you met "unsupported type" compile error: (1)
  */
  auto task = actor.Send<&Counter::Add>(1);

  /*
  3.1 For local actors, you can try `SendLocal`, which doesn't require the args to be serializable.
  */
  auto sender = actor.SendLocal<&Counter::Add>(1);

  /*
  4. The task is lazy executed. To execute the task and wait for the result non-blockingly,
  use `co_await` (2). Note that the task is not copyable, so you need to use `std::move`.
  */
  auto res = co_await std::move(task);
  assert(res == 1);

  // A shorter way
  res = co_await actor.Send<&Counter::Add>(1);
  assert(res == 2);
}


int main() {
  // use the thread pool inside ActorRegistry to execute the coroutine.
  stdexec::sync_wait(stdexec::starts_on(registry.GetScheduler(), MainCoroutine()));
}
```
<!-- doc test end -->

1.  This method requires your args can be serialized by reflect-cpp, because it can potentially be passed through the network in distributed mode.
    
    reflect-cpp can handle simple structs and common containers automatically, but if your type is non-trivial(e.g. has private fields),
    you may meet compile errors like "Unsupported type", please refer <https://rfl.getml.com/concepts/custom_classes>
    to add a serializer for it.
  
    **If you only run ex_actor in a single process, you can use `SendLocal()` instead, which doesn't require the args to be serializable, see below.**

2.  `co_await` is non-blocking, the thread will be able to do other work while waiting for the result.

    In this example, the main thread has no other work to do and will still be blocked in `sync_wait`.
    But if you schedule the coroutine in a scheduler,
    the thread of the scheduler can do other work while waiting for the result.


## Send message from one actor to another


When calling an actor's method from another actor, you should make the method a coroutine.

This example shows how to call an actor's method from another actor without blocking the scheduler thread.


<!-- doc test start -->
```cpp
#include <cassert>
#include <iostream>
#include "ex_actor/api.h"

class PingWorker {
 public:
  std::string Ping() { return "Hi"; }
};

class Proxy {
 public:
  explicit Proxy(ex_actor::ActorRef<PingWorker> actor_ref) : actor_ref_(actor_ref) {}
  
  // Actor's method can be a coroutine.
  exec::task<std::string> ProxyPing() {
    // This line won't block the scheduler thread.
    std::string ping_res = co_await actor_ref_.template Send<&PingWorker::Ping>();
    co_return ping_res + " from Proxy";
  }

 private:
  ex_actor::ActorRef<PingWorker> actor_ref_;
};

// here we have only one thread in scheduler, but it still can finish the entire work,
// because we use coroutine, there is no blocking wait in actor's method.
ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);

exec::task<void> MainCoroutine() {
  ex_actor::ActorRef ping_worker = co_await registry.CreateActor<PingWorker>();

  // 1. create a proxy actor, who has a reference to the ping_worker actor
  ex_actor::ActorRef proxy = co_await registry.CreateActor<Proxy>(ping_worker);

  // 2. call through the proxy actor.
  // When the return type of your method is a std::execution sender, we'll automatically
  // unwrap the result for you, so you don't need to `co_await` twice.
  std::string res = co_await proxy.Send<&Proxy::ProxyPing>();
  assert(res == "Hi from Proxy");
}

int main() {
  stdexec::sync_wait(stdexec::starts_on(registry.GetScheduler(), MainCoroutine()));
}
```
<!-- doc test end -->


## Create actors inside an actor

If you want to create an actor inside an actor, it will be handy to make ActorRegistry a global variable.
All APIs of ActorRegistry are thread-safe.

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

// Again, here we have only one thread in scheduler, but it still can finish the entire work,
// because we use coroutine, there is no blocking wait in actor's method.
ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);

class Child {
public:
  std::string Ping() {
    return "Dad, I'm here!";
  }
};

class Father {
public:
  // Actor's method can be a coroutine.
  exec::task<std::string> SpawnChildAndPing() {
    if (child_.IsEmpty()) {
      // this line won't block the scheduler thread
      child_ = co_await registry.CreateActor<Child>();
    }
    // this line won't either
    std::string child_res = co_await child_.Send<&Child::Ping>();
    co_return "Where is my child? " + child_res;
  }
private:
  ex_actor::ActorRef<Child> child_;
};

exec::task<void> MainCoroutine() {
  ex_actor::ActorRef<Father> father = co_await registry.CreateActor<Father>();
  // When the return type of your method is a std::execution sender, we'll automatically
  // unwrap the result for you, so you don't need to `co_await` twice.
  std::string res = co_await father.Send<&Father::SpawnChildAndPing>();
  assert(res == "Where is my child? Dad, I'm here!");
}

int main() {
  stdexec::sync_wait(stdexec::starts_on(registry.GetScheduler(), MainCoroutine()));
}
```
<!-- doc test end -->

## Execute multiple tasks in parallel

You can execute multiple tasks in parallel using [`when_all`](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html#design-sender-adaptor-when_all) or [`async_scope`](https://kirkshoop.github.io/async_scope/asyncscope.html) in std::execution.

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

ex_actor::ActorRegistry registry(/*thread_pool_size=*/3);

struct Counter {
  int AddAndGet(int x) { return count += x; }
  void Add(int x) { count += x; }
  int GetValue() const { return count; }
  int count = 0;
};

exec::task<void> MainCoroutine() {
  // create multiple counters, you want to increase them in parallel
  std::vector<ex_actor::ActorRef<Counter>> counters;
  for (int i = 0; i < 3; ++i) {
    counters.push_back(co_await registry.CreateActor<Counter>());
  }

  // `when_all` example, handy for small number of tasks.
  auto [res1, res2, res3] = co_await stdexec::when_all(
    counters[0].Send<&Counter::AddAndGet>(1),
    counters[1].Send<&Counter::AddAndGet>(2),
    counters[2].Send<&Counter::AddAndGet>(3)
  );
  assert(res1 == 1);
  assert(res2 == 1);
  assert(res3 == 1);

  // for large number of tasks where you need a loop, use `async_scope`.
  exec::async_scope scope;

  // `async_scope.spawn_future` example, which returns a future-like object which you can wait for later.
  using FutureType = decltype(scope.spawn_future(counters[0].Send<&Counter::AddAndGet>(1)));
  std::vector<FutureType> futures;
  for (int i = 0; i < counters.size(); ++i) {
    auto future = scope.spawn_future(counters[i].Send<&Counter::AddAndGet>(1));
    futures.push_back(std::move(future));
  }
  co_await scope.on_empty();
  for (int i = 0; i < futures.size(); ++i) {
    int value = co_await std::move(futures[i]);
    assert(value == 2);
  }

  // async_scope.spawn example, which only accepts void tasks.
  for (int i = 0; i < counters.size(); ++i) {
    scope.spawn(counters[i].Send<&Counter::Add>(1));
  }
  co_await scope.on_empty();
  for (int i = 0; i < counters.size(); ++i) {
    int value = co_await counters[i].Send<&Counter::GetValue>();
    assert(value == 3);
  }
}

int main() {
  stdexec::sync_wait(stdexec::starts_on(registry.GetScheduler(), MainCoroutine()));
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

struct Counter {
  int Add(int x) { return count += x; }
  int count = 0;
};

int main() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);
  auto [actor] = stdexec::sync_wait(registry.CreateActor<Counter>()).value();

  // Sender adapter style
  int local_var = 1;
  auto task1 = actor.Send<&Counter::Add>(1) | stdexec::then([&local_var](int value) {
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

In the above example, the `ex::then` callback runs on the actor's thread. If you capture local variables in the `ex::then` callback, it will cause a data race.

A more dangerous example is capturing `this` in actor's method.

<!-- doc test start -->
```cpp
#include <cassert>
#include <iostream>
#include "ex_actor/api.h"

ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);

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

exec::task<void> MainCoroutine() {
  ex_actor::ActorRef dummy_actor = co_await registry.CreateActor<DummyActor>();

  // 2. create a proxy actor, who has a reference to the dummy actor
  ex_actor::ActorRef proxy = co_await registry.CreateActor<Proxy>(dummy_actor);

  // 3. call through the proxy actor
  exec::async_scope scope;
  scope.spawn(proxy.Send<&Proxy::SomeMethod>());
  scope.spawn(proxy.Send<&Proxy::AnotherMethod>());
  co_await scope.on_empty();
}

int main() {
  stdexec::sync_wait(stdexec::starts_on(registry.GetScheduler(), MainCoroutine()));
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
