# Quick Start

## Creating an actor

Assume you have a class, you want to turn it into an actor:

```cpp
struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};
```

First, select a `std::execution` scheduler you like, if you have no idea, we recommend the work-sharing thread pool
we provide, which is suitable for most cases.
```cpp
#include "ex_actor/api.h"

ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/10);
auto scheduler = thread_pool.GetScheduler();
```

Then, create an actor registry using the scheduler, and use the registry to create an actor:
```cpp
ex_actor::ActorRegistry registry(scheduler);
ex_actor::ActorRef actor = registry.CreateActor<YourClass>();
```

That's all, everything is setup, you can call the actor's method now:
```cpp
auto sender = actor.Send<&YourClass::Add>(1);
```
The method returns a standard `std::execution` sender, compatible with everything in the `std::execution` ecosystem.
For example, you can `co_await` it, or use `ex::then` to wrap it, etc.

To execute the sender and consume the result, use [`sync_wait`](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html#design-sender-consumer-sync_wait):
```cpp
auto [res] = stdexec::sync_wait(sender).value();
```

## How actor interacts with the scheduler

```d2
direction: right

caller -> actor.mailbox: 1. Send message
caller.shape: circle

actor
actor.mailbox -> actor.user class: 3. pull & execute
actor.mailbox.shape: queue

scheduler
actor -> scheduler: 2. activate
scheduler.task queue.shape: queue
scheduler.worker thread

```

We wrap your class into an actor, the actor contains a mailbox(a queue),
whenever a message is pushed to the mailbox, the actor will be activated - pushed to the scheduler.

You can think of pushing the following lambda to the scheduler:

```cpp
scheduler.push_task([actor = std::move(actor)] {
  int message_executed = 0;
  while (!actor.mailbox.empty()) {
    auto message = actor.mailbox.pop();
    message->Execute();
    message_executed++;
    if (message_executed >= actor.max_message_executed_per_activation) {
      break;
    }
  }
});
```

We'll handle the synchronization correctly, so that **at any time, there is at most one thread executing the actor**.
So you don't need to worry about the synchronization when writing actor methods.