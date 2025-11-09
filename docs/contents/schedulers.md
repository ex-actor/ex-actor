# Schedulers

We provide some std::execution schedulers out-of-box:

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