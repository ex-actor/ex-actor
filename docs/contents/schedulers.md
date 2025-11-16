# Schedulers

You can use any std::execution scheduler as ex_actor's underlying scheduler, just pass it to the `ActorRegistry` constructor. By default, we use `ex_actor::WorkSharingThreadPool`.

```cpp
#include "ex_actor/api.h"

ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/10);
ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

// which is identical to:
ex_actor::ActorRegistry registry(/*thread_pool_size=*/10);
```

We provide some handy schedulers out-of-box, check them below.

If you are interested in the details, you can read [understanding how actor is scheduled](#understanding-how-actor-is-scheduled) section to understand how ex_actor use a std::execution scheduler to schedule actors.

## Work-Sharing Thread Pool

```cpp
#include "ex_actor/api.h"

ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/10);
auto scheduler = thread_pool.GetScheduler();
```

This scheduler is suitable for most cases.

It's a classic thread pool with a shared lock-free task queue.

```d2
direction: right
caller -> scheduler.task queue: schedule task
scheduler.task queue.shape: queue
scheduler.task queue->scheduler.worker thread 1
scheduler.task queue->scheduler.worker thread 2
scheduler.task queue->scheduler.worker thread 3

```

## Work-Stealing Thread Pool

```cpp
#include "ex_actor/api.h"

ex_actor::WorkStealingThreadPool thread_pool(/*thread_count=*/10);
auto scheduler = thread_pool.GetScheduler();
```

It's an alias of `stdexec`'s `exec::static_thread_pool`, which is a sophisticated work-stealing-style thread pool.
Every thread has a LIFO local task queue, and when a thread is idle, it will steal tasks from other threads.

It has better performance in some cases. But the task stealing overhead can be non-negligible in some low-latency scenarios.
Use it when you know what you are doing.

```d2
direction: right
caller -> scheduler.task queue 1
caller -> scheduler.task queue 2
caller -> scheduler.task queue 3
scheduler.task queue 1.shape: queue
scheduler.task queue 2.shape: queue
scheduler.task queue 3.shape: queue
scheduler.task queue 1->scheduler.worker thread 1
scheduler.task queue 2->scheduler.worker thread 2
scheduler.task queue 3->scheduler.worker thread 3
```

## Priority Thread Pool

<!-- doc test start -->
```cpp
#include "ex_actor/api.h"

struct TestActor {
  void Foo() {}
};

int main() {
  ex_actor::PriorityThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  ex_actor::ActorRegistry registry(scheduler);
  auto actor = registry.CreateActor<TestActor>(ex_actor::ActorConfig {
    .priority = 1 // smaller number means higher priority
  });
}
```
<!-- doc test end -->

It's a thread pool with a priority queue. You can set the priority of an actor when creating it.
When an actor is activated, it will be pushed to the scheduler with its priority.
The scheduler will execute the tasks with higher priority(smaller number) first.


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
actors run in a dedicated thread pool, so it won't be blocked by the data-processing actors.

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