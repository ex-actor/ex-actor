# Schedulers

You can use any std::execution scheduler as ex_actor's underlying scheduler, just pass it to the `ActorRegistry` constructor.
When you pass a scheduler to the `ActorRegistry` constructor, we'll use it as the underlying scheduler, instead of using our
default scheduler.

```cpp
#include "ex_actor/api.h"

ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/10);

// pass the scheduler to the registry
ex_actor::ActorRegistry registry(thread_pool.GetScheduler()); 
```

We provide some handy schedulers out-of-box, check them below.

If you are interested in the details, you can read [understanding how actor is scheduled](#understanding-how-actor-is-scheduled) section to understand how ex_actor use a std::execution scheduler to schedule actors.

## Work-Sharing Thread Pool

```cpp
#include "ex_actor/api.h"

ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/10);
ex_actor::ActorRegistry registry(thread_pool.GetScheduler()); 
```

This scheduler is suitable for most cases. It's a classic thread pool with a globally shared lock-free task queue.

It's also the default scheduler we use when you don't pass a scheduler to the `ActorRegistry` constructor.


## Work-Stealing Thread Pool

```cpp
#include "ex_actor/api.h"

ex_actor::WorkStealingThreadPool thread_pool(/*thread_count=*/10);
ex_actor::ActorRegistry registry(thread_pool.GetScheduler()); 
```

It's an alias of `stdexec`'s `exec::static_thread_pool`, which is a sophisticated [work-stealing-style](https://en.wikipedia.org/wiki/Work_stealing) thread pool.
Every thread has a LIFO local task queue, and when a thread is idle, it will steal tasks from other threads.

It has better performance in some cases. But the task stealing overhead can be non-negligible in some low-latency scenarios.
Use it when you know what you are doing.

## Priority Thread Pool

<!-- doc test start -->
```cpp
#include "ex_actor/api.h"

struct TestActor {
  void Foo() {}
};

int main() {
  ex_actor::PriorityThreadPool thread_pool(1);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
  auto actor = registry.CreateActor<TestActor>(ex_actor::ActorConfig {
    .priority = 1 // smaller number means higher priority
  });
}
```
<!-- doc test end -->

It's a thread pool with a lock-guarded priority queue. You can set the priority of an actor when creating it.
When an actor is activated, it will be pushed to the scheduler with its priority.
The scheduler will execute the tasks with higher priority(smaller number) first.

In practice it's used to prioritize downstream actors in some high-throughput systems, so that there won't be a lot of data pending in the middle of the pipeline, reducing the memory pressure.

Event though this scheduler takes priority into account, the actor scheduling is still cooperative. Which means a thread can't be interrupted and switch to higher priority actors when executing an actor's message.
If you do have some very high-priority actors, consider using the [`SchedulerUnion`](#scheduler-union) scheduler to put them in a dedicated thread pool.

## Scheduler Union

<!-- doc test start -->
```cpp
#include "ex_actor/api.h"
#include <cassert>

struct TestActor {
  uint64_t GetThreadId() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }
};
int main() {
  ex_actor::WorkSharingThreadPool thread_pool1(1);
  ex_actor::WorkSharingThreadPool thread_pool2(1);
  ex_actor::SchedulerUnion scheduler_union(std::vector<ex_actor::WorkSharingThreadPool::Scheduler> {
    thread_pool1.GetScheduler(), thread_pool2.GetScheduler()
  });
  auto scheduler = scheduler_union.GetScheduler();
  ex_actor::ActorRegistry registry(scheduler);

  auto actor1 = registry.CreateActor<TestActor>(ex_actor::ActorConfig {.scheduler_index = 0});
  auto actor2 = registry.CreateActor<TestActor>(ex_actor::ActorConfig {.scheduler_index = 1});

  auto [thread_id1] = stdexec::sync_wait(actor1.Send<&TestActor::GetThreadId>()).value();
  auto [thread_id2] = stdexec::sync_wait(actor2.Send<&TestActor::GetThreadId>()).value();
  assert(thread_id1 != thread_id2);
}
```
<!-- doc test end -->

It's a scheduler that combines multiple schedulers. You can set the scheduler index of an actor when creating it.
When an actor is activated, it will be pushed to the specified scheduler.

It's useful when you want to split actors by groups. E.g. some control-flow actors and some data-processing actors, and you want the control-flow
actors run in a dedicated thread pool, scheduled preemptively with the other group, so it won't be blocked by the data-processing actors.

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