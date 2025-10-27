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

// 1. Assume you have a class, you want to turn it into an actor.
struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};


int main() {
  /*
  2. First, select a `std::execution` scheduler you like, if you have
  no idea, we recommend `ex_actor::WorkSharingThreadPool` we provide,
  which is suitable for most cases.
  */
  ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/2);
  auto scheduler = thread_pool.GetScheduler();

  /*
  3. Then, create an actor registry using the scheduler, and use the
  registry to create an actor.
  */
  ex_actor::ActorRegistry registry(scheduler);
  ex_actor::ActorRef actor = registry.CreateActor<YourClass>();
  
  /*
  4. Everything is setup, you can call the actor's method now.
  
  The method returns a standard `std::execution::task`, compatible
  with everything in the `std::execution` ecosystem.

  This method requires your args can be serialized by reflect-cpp, if you met compile
  errors like "Unsupported type", refer https://rfl.getml.com/concepts/custom_classes/
  to add a serializer for it.
  
  **Or if you can confirm it's a local actor, use SendLocal() instead. See below.**
  */
  auto task = actor.Send<&YourClass::Add>(1);

  /*
  4.1 For local actors, you can try `SendLocal`, which has better performance,
  and don't require the args to be serializable.
  */
  auto sender = actor.SendLocal<&YourClass::Add>(1);

  /*
  5. To execute the task and consume the result, use `sync_wait`.
  Note that the task is not copyable, so you need to use `std::move`.
  */
  auto [res] = stdexec::sync_wait(std::move(task)).value();
  assert(res == 1);
}
```
<!-- doc test end -->

## Wrap the result in an async way

Through this example, you'll learn how to wrap the sender using sender adapters and coroutines.
You'll also learn about the scheduler switching mechanism in `std::execution`.

**This part is the hardest and most important part of this tutorial. Please read it carefully.**

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};

int main() {
  ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/2);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
  ex_actor::ActorRef actor = registry.CreateActor<YourClass>();

  // --- Example 1: Use `then` to wrap the sender ---
  auto task1 = actor.Send<&YourClass::Add>(1) | stdexec::then([](int value) {
    // this line will be executed on the actor's thread.
    return value + 1;
  });
  auto [res1] = stdexec::sync_wait(std::move(task1)).value();
  assert(res1 == 2);


  // --- Example 2: Coroutine ---
  auto coroutine = [&actor]() -> exec::task<int> {
    auto res = co_await actor.Send<&YourClass::Add>(1);
    /*
    the following line will be executed on the caller's thread
    - here is the main thread. see below for more details.
    */
    assert(res == 1);
    co_return res + 2;
  };
  auto [res2] = stdexec::sync_wait(coroutine()).value();
  assert(res2 == 3);
}
```
<!-- doc test end -->


### Understanding the scheduler switching

You may be curious about why the first example's callback runs on the actor's thread. While the second example's callback(code after `co_await`) runs on the caller's thread.

To understand this, you need to know the scheduler switching mechanism in `std::execution`.

In `std::execution`, scheduler's switch should be explicit - by calling `continue_on` explicitly.

An actor itself is a scheduler (not the scheduler passed to the `ActorRegistry` constructor, but **actor itself**), when you call its method, you schedule a task on it.
So all the callbacks will run on the actor's thread.

But in a coroutine, the code **looks like** they are executing in the same thread.
So in order not to confuse the user, make coroutine easy to use, `std::execution::task` has **scheduler affinity** - it will keep the scheduler the same across the entire coroutine.
In other words, after any `co_await` in the coroutine, `std::execution::task` will help you to switch back to the coroutine's scheduler.
(See [`std::execution::task`'s proposal](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3552r3.html) for more details).

In the second example, the coroutine's scheduler is the `run_loop` scheduler in `sync_wait`, which is the main thread. So after `co_await sender`, the coroutine will switch back to the main thread.



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
  
  // coroutine style
  exec::task<int> GetValue1() {
    int res = co_await actor_ref_.template Send<&Counter::GetValue>();
    std::cout << "This line runs on the current actor(Proxy), "
                 "because coroutine has scheduler affinity.\n";
    co_return res;
  }

  // sender adapter style
  stdexec::sender auto GetValue2() {
    return actor_ref_.template Send<&Counter::GetValue>() | stdexec::then([](int value) {
             std::cout << "This line runs on the target actor(Counter), "
                          "unless you call continue_on explicitly.\n";
             return value;
           });
  }

 private:
  ex_actor::ActorRef<Counter> actor_ref_;
};

int main() {
  ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/2);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
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
  auto [res2] = stdexec::sync_wait(proxy.Send<&Proxy::GetValue1>()).value();
  auto [res3] = stdexec::sync_wait(proxy.Send<&Proxy::GetValue2>()).value();
  assert(res2 == 100);
  assert(res3 == 100);
}
```
<!-- doc test end -->

Read the [previous section](#understanding-the-scheduler-switching) if you can't understand why `GetValue2`'s call back runs on the target actor(Counter), while `GetValue1`'s call back runs on the current actor(Proxy).

## Understanding how actor is scheduled

Now you've learnt the basic usage of `ex_actor`. Next we'll dig a little deep, to understand how an actor is scheduled.

This part is optional, if you don't want to know the details, you can skip it. But if you have time we
recommend you to read it to have a better understanding of how actor works.

```d2
direction: right

caller -> actor.mailbox: 1.Send message
caller.shape: circle

actor
actor.mailbox -> actor.user class: 3.pull & execute
actor.mailbox.shape: queue

scheduler
actor -> scheduler: 2.activate
scheduler.task queue.shape: queue
scheduler.worker thread

```

We wrap your class into an actor, the actor contains a mailbox(a queue),
whenever a message is pushed to the mailbox, the actor will be activated - pushed to the scheduler.

You can think of pushing the following pseudo lambda to the scheduler:

```cpp
// pseudo code of actor activation
scheduler.push_task([actor = std::move(actor)] {
  int message_executed = 0;
  while (!actor.mailbox.empty()) {
    auto message = actor.mailbox.pop();
    message->Execute();
    message_executed++;
    /*
    we limit the number of messages executed per activation
    so that other actors won't starve.
    */
    if (message_executed >= actor.max_message_executed_per_activation) {
      break;
    }
  }
  if (still has messages in the mailbox) {
    push again to the scheduler
  }
});
```

We'll handle the synchronization correctly, so that **at any time, there is at most one thread executing the actor**.
So you don't need to worry about the synchronization when writing actor methods.

The whole schedule process is like this:

1. someone calls an actor's method - i.e. start a sender returned by `actor.Send<>`.
2. we push this message(the target method & its callbacks) to the actor's mailbox.
3. we check if the actor is activated, if not, we activate it, push an activation task(see the above pseudo code) to the scheduler.
4. the scheduler get the task, execute it, in which the actor will pull messages from its mailbox and execute them.
5. the actor runs out of messages, or max messages executed per activation is reached, the activation task finishes.
6. if there are still messages in the mailbox, the activation task will be pushed again to the scheduler.