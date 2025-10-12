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
