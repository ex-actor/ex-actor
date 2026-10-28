#include <algorithm>
#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_config.h"

namespace ex = ex_actor::ex;

template <class ThreadPool>
class ThreadPoolConfigTest : public testing::Test {};

using ConfigurableThreadPools = testing::Types<ex_actor::WorkSharingThreadPool, ex_actor::PriorityThreadPool,
                                               ex_actor::CorePinnedThreadPool, ex_actor::WeakPriorityThreadPool>;
TYPED_TEST_SUITE(ThreadPoolConfigTest, ConfigurableThreadPools);

TYPED_TEST(ThreadPoolConfigTest, StartsWorkersByDefault) {
  TypeParam thread_pool(typename TypeParam::Config {});
  auto [worker_id] =
      ex::sync_wait(ex::schedule(thread_pool.GetScheduler()) | ex::then([]() { return std::this_thread::get_id(); }))
          .value();
  EXPECT_NE(worker_id, std::this_thread::get_id());
}

TYPED_TEST(ThreadPoolConfigTest, DeferredStartupExecutesQueuedAndNewTasks) {
  TypeParam thread_pool({.thread_count = 2, .start_workers_immediately = false});
  std::atomic_int count = 0;
  stdexec::simple_counting_scope scope;
  std::latch workers_ready(/*expected=*/2);
  for (size_t core_index = 0; core_index < 2; ++core_index) {
    auto sender = ex::schedule(thread_pool.GetScheduler()) | ex::then([&]() {
                    ++count;
                    // Require both configured workers to run, including separate core-pinned queues.
                    workers_ready.arrive_and_wait();
                  }) |
                  ex::write_env(ex::prop {ex_actor::get_core_index, core_index});
    stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(count, 0);
  thread_pool.StartWorkers();
  ex::sync_wait(scope.join());
  EXPECT_EQ(count, 2);
  ex::sync_wait(ex::schedule(thread_pool.GetScheduler()) | ex::then([&]() { ++count; }));
  EXPECT_EQ(count, 3);
}

TYPED_TEST(ThreadPoolConfigTest, DeferredQueuedTasksCanBeStopped) {
  TypeParam thread_pool({.thread_count = 1, .start_workers_immediately = false});
  std::atomic_int count = 0;
  stdexec::counting_scope scope;
  auto sender = ex::schedule(thread_pool.GetScheduler()) | ex::then([&]() { ++count; });
  stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
  scope.request_stop();
  thread_pool.StartWorkers();
  ex::sync_wait(scope.join());
  EXPECT_EQ(count, 0);
}

TYPED_TEST(ThreadPoolConfigTest, CanBeDestroyedWithoutStartingWorkers) {
  TypeParam thread_pool({.thread_count = 2, .start_workers_immediately = false});
}

TEST(SchedulerTest, WeakPrioritySubQueueConfiguration) {
  for (size_t num_sub_queues : {0, 1, 7}) {
    ex_actor::WeakPriorityThreadPool thread_pool(
        {.thread_count = 1, .start_workers_immediately = false, .num_sub_queues = num_sub_queues});
    std::atomic_int count = 0;
    stdexec::simple_counting_scope scope;
    for (uint32_t priority = 0; priority < ex_actor::WeakPriorityThreadPool::kMaxPriorityLevels; ++priority) {
      auto sender = ex::schedule(thread_pool.GetScheduler()) | ex::then([&]() { ++count; }) |
                    ex::write_env(ex::prop {ex_actor::get_priority, priority});
      stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
    }
    thread_pool.StartWorkers();
    ex::sync_wait(scope.join());
    EXPECT_EQ(count, ex_actor::WeakPriorityThreadPool::kMaxPriorityLevels);
  }
}

class WorkSharingConfigTest : public testing::TestWithParam<bool> {};

INSTANTIATE_TEST_SUITE_P(BlockingAndSpinning, WorkSharingConfigTest, testing::Bool());

TEST(SchedulerTest, WorkSharingLegacyConstructorCanDeferStartup) {
  ex_actor::WorkSharingThreadPool thread_pool(/*thread_count=*/1, /*start_workers_immediately=*/false);
  std::atomic_bool executed = false;
  stdexec::simple_counting_scope scope;
  auto sender = ex::schedule(thread_pool.GetScheduler()) | ex::then([&]() { executed = true; });
  stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
  EXPECT_FALSE(executed);
  thread_pool.StartWorkers();
  ex::sync_wait(scope.join());
  EXPECT_TRUE(executed);
}

TEST_P(WorkSharingConfigTest, DeferredStartupAndLocalScheduling) {
  ex_actor::WorkSharingThreadPool thread_pool(
      {.thread_count = 1, .start_workers_immediately = false, .spin = GetParam()});
  auto scheduler = thread_pool.GetScheduler();
  std::atomic_int count = 0;
  stdexec::simple_counting_scope scope;
  auto sender = ex::schedule(scheduler) | ex::then([&]() { ++count; }) |
                ex::let_value([&]() { return ex::schedule(scheduler) | ex::then([&]() { ++count; }); });
  stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
  EXPECT_EQ(count, 0);
  thread_pool.StartWorkers();
  ex::sync_wait(scope.join());
  EXPECT_EQ(count, 2);

  // Exercise an empty queue followed by a new submission, then idle destruction.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ex::sync_wait(ex::schedule(scheduler) | ex::then([&]() { ++count; }));
  EXPECT_EQ(count, 3);
}

TEST_P(WorkSharingConfigTest, QueuedTasksCanBeStopped) {
  ex_actor::WorkSharingThreadPool thread_pool(
      {.thread_count = 1, .start_workers_immediately = false, .spin = GetParam()});
  std::atomic_int count = 0;
  stdexec::counting_scope scope;
  auto sender = ex::schedule(thread_pool.GetScheduler()) | ex::then([&]() { ++count; });
  for (int i = 0; i < 100; ++i) {
    stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
  }
  scope.request_stop();
  thread_pool.StartWorkers();
  ex::sync_wait(scope.join());
  EXPECT_EQ(count, 0);
}

#if defined(__linux__)
TEST_P(WorkSharingConfigTest, WorkersUseConfiguredCoresRoundRobin) {
  cpu_set_t available_cores;
  ASSERT_EQ(pthread_getaffinity_np(pthread_self(), sizeof(available_cores), &available_cores), 0);
  std::vector<size_t> cores;
  for (size_t core_index = 0; core_index < CPU_SETSIZE && cores.size() < 2; ++core_index) {
    if (CPU_ISSET(core_index, &available_cores)) {
      cores.push_back(core_index);
    }
  }
  ASSERT_FALSE(cores.empty());
  ex_actor::WorkSharingThreadPool thread_pool({.thread_count = 3, .spin = GetParam(), .cores = cores});
  std::latch workers_ready(/*expected=*/3);
  std::vector<int> worker_cores(/*count=*/3, /*value=*/-1);
  stdexec::simple_counting_scope scope;
  for (size_t task_index = 0; task_index < worker_cores.size(); ++task_index) {
    auto sender = ex::schedule(thread_pool.GetScheduler()) | ex::then([&, task_index]() {
                    cpu_set_t affinity;
                    int result = pthread_getaffinity_np(pthread_self(), sizeof(affinity), &affinity);
                    EXPECT_EQ(result, 0);
                    if (result == 0) {
                      EXPECT_EQ(CPU_COUNT(&affinity), 1);
                    }
                    worker_cores[task_index] = sched_getcpu();
                    // Keep each worker occupied so every thread's affinity is checked.
                    workers_ready.arrive_and_wait();
                  });
    stdexec::spawn(sender | ex_actor::DiscardResult(), scope.get_token());
  }
  ex::sync_wait(scope.join());
  std::vector<int> expected_cores;
  for (size_t thread_index = 0; thread_index < worker_cores.size(); ++thread_index) {
    expected_cores.push_back(static_cast<int>(cores[thread_index % cores.size()]));
  }
  std::sort(worker_cores.begin(), worker_cores.end());
  std::sort(expected_cores.begin(), expected_cores.end());
  EXPECT_EQ(worker_cores, expected_cores);
}
#endif

TEST(SchedulerTest, TaskInWorkSharingThreadPoolShouldBeStoppable) {
  ex_actor::WorkSharingThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  auto task = ex::schedule(scheduler) | ex::then([]() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
  stdexec::counting_scope scope;
  for (int i = 0; i < 100000; ++i) {
    stdexec::spawn(task | ex_actor::DiscardResult(), scope.get_token());
  }
  scope.request_stop();
  ex::sync_wait(scope.join());
}

struct TestActor {
  void Foo() {
    count++;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  int count = 0;
};
TEST(SchedulerTest, ActorTaskShouldBeStoppable) {
  ex_actor::WorkSharingThreadPool thread_pool(1);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
  auto [actor] = stdexec::sync_wait(registry.Spawn<TestActor>()).value();
  stdexec::counting_scope scope;
  for (int i = 0; i < 100000; ++i) {
    stdexec::spawn(actor.Send<&TestActor::Foo>() | ex_actor::DiscardResult(), scope.get_token());
  }
  scope.request_stop();
  ex::sync_wait(scope.join());
}

TEST(SchedulerTest, PrioritySchedulerTest) {
  ex_actor::PriorityThreadPool thread_pool(1, /*start_workers_immediately=*/false);
  auto scheduler = thread_pool.GetScheduler();
  std::atomic_int count = 0;
  auto sender1 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 0);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ex_actor::get_priority, 1});
  auto sender2 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 2);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ex_actor::get_priority, 3});
  auto sender3 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 1);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ex_actor::get_priority, 2});
  stdexec::simple_counting_scope scope;
  stdexec::spawn(sender1 | ex_actor::DiscardResult(), scope.get_token());
  stdexec::spawn(sender2 | ex_actor::DiscardResult(), scope.get_token());
  stdexec::spawn(sender3 | ex_actor::DiscardResult(), scope.get_token());
  thread_pool.StartWorkers();
  ex::sync_wait(scope.join());
  ASSERT_EQ(count, 3);
}

struct TestActor2 {
  uint64_t GetThreadId() { return std::hash<std::thread::id> {}(std::this_thread::get_id()); }
};

TEST(SchedulerTest, SchedulerUnionTest) {
  ex_actor::WorkSharingThreadPool thread_pool1(1);
  ex_actor::WorkSharingThreadPool thread_pool2(1);
  ex_actor::SchedulerUnion scheduler_union(thread_pool1.GetScheduler(), thread_pool2.GetScheduler());
  auto scheduler = scheduler_union.GetScheduler();
  auto start = ex::schedule(scheduler) | ex::then([] { return std::this_thread::get_id(); });

  auto sender1 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 0});
  auto sender2 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 1});
  auto [thread_id1] = ex::sync_wait(sender1).value();
  auto [thread_id2] = ex::sync_wait(sender2).value();
  ASSERT_NE(thread_id1, thread_id2);

  auto coroutine = [&]() -> stdexec::task<void> {
    // create two actors, specify the scheduler index in ActorConfig.
    auto actor1 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.scheduler_index = 0});
    auto actor2 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.scheduler_index = 1});

    uint64_t thread_id1 = co_await actor1.Send<&TestActor2::GetThreadId>();
    uint64_t thread_id2 = co_await actor2.Send<&TestActor2::GetThreadId>();
    // the two actors should run on different thread pool
    EXPECT_NE(thread_id1, thread_id2);
  };
  ex_actor::Init(scheduler_union.GetScheduler());
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SchedulerTest, SchedulerUnionHeterogeneousTest) {
  // Mix two different scheduler types to prove variadic SchedulerUnion works.
  ex_actor::WorkSharingThreadPool ws_pool(1);
  ex_actor::PriorityThreadPool prio_pool(1);
  ex_actor::SchedulerUnion scheduler_union(ws_pool.GetScheduler(), prio_pool.GetScheduler());
  auto scheduler = scheduler_union.GetScheduler();
  auto start = ex::schedule(scheduler) | ex::then([] { return std::this_thread::get_id(); });

  auto sender1 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 0});
  auto sender2 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 1});
  auto [thread_id1] = ex::sync_wait(sender1).value();
  auto [thread_id2] = ex::sync_wait(sender2).value();
  ASSERT_NE(thread_id1, thread_id2);
}

TEST(SchedulerTest, TestResourceHolder) {
  auto shared_pool1 = std::make_unique<ex_actor::WorkSharingThreadPool>(10);
  auto shared_pool2 = std::make_unique<ex_actor::WorkSharingThreadPool>(10);
  auto union_pool = std::make_unique<
      ex_actor::SchedulerUnion<ex_actor::WorkSharingThreadPool::Scheduler, ex_actor::WorkSharingThreadPool::Scheduler>>(
      shared_pool1->GetScheduler(), shared_pool2->GetScheduler());
  ex_actor::Init(union_pool->GetScheduler());
  ex_actor::HoldResource(std::move(shared_pool1));
  ex_actor::HoldResource(std::move(shared_pool2));
  ex_actor::HoldResource(std::move(union_pool));
  auto coroutine = [&]() -> stdexec::task<void> {
    // create two actors, specify the scheduler index in ActorConfig.
    auto actor1 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.scheduler_index = 0});
    auto actor2 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.scheduler_index = 1});

    uint64_t thread_id1 = co_await actor1.Send<&TestActor2::GetThreadId>();
    uint64_t thread_id2 = co_await actor2.Send<&TestActor2::GetThreadId>();
    // the two actors should run on different thread pool
    EXPECT_NE(thread_id1, thread_id2);
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SchedulerTest, CorePinnedThreadPoolTest) {
  ex_actor::CorePinnedThreadPool thread_pool(2);
  auto scheduler = thread_pool.GetScheduler();
  auto start = ex::schedule(scheduler) | ex::then([] { return std::this_thread::get_id(); });

  auto sender1 = start | ex::write_env(ex::prop {ex_actor::get_core_index, 0});
  auto sender2 = start | ex::write_env(ex::prop {ex_actor::get_core_index, 1});

  auto [thread_id1] = ex::sync_wait(sender1).value();
  auto [thread_id2] = ex::sync_wait(sender2).value();
  ASSERT_NE(thread_id1, thread_id2);

  auto coroutine = [&]() -> stdexec::task<void> {
    auto actor1 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.core_index = 0});
    auto actor2 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.core_index = 1});

    uint64_t thread_id1 = co_await actor1.Send<&TestActor2::GetThreadId>();
    uint64_t thread_id2 = co_await actor2.Send<&TestActor2::GetThreadId>();
    EXPECT_NE(thread_id1, thread_id2);
  };
  ex_actor::Init(scheduler);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SchedulerTest, WeakPriorityThreadPoolStoppableTest) {
  ex_actor::WeakPriorityThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  auto task = ex::schedule(scheduler) | ex::then([]() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
  stdexec::counting_scope scope;
  for (int i = 0; i < 100000; ++i) {
    stdexec::spawn(task | ex::write_env(ex::prop {ex_actor::get_priority, static_cast<uint32_t>(i % 4)}) |
                       ex_actor::DiscardResult(),
                   scope.get_token());
  }
  scope.request_stop();
  ex::sync_wait(scope.join());
}

TEST(SchedulerTest, WeakPriorityThreadPoolLocalSlotTest) {
  // Verify the thread-local slot works: an operation that re-enqueues to the same pool
  // should complete without deadlock.
  ex_actor::WeakPriorityThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  auto sender = ex::schedule(scheduler) | ex::then([]() { return 1; }) |
                ex::write_env(ex::prop {ex_actor::get_priority, 0}) | ex::let_value([&](int val) {
                  return ex::schedule(scheduler) | ex::then([val]() { return val + 1; }) |
                         ex::write_env(ex::prop {ex_actor::get_priority, 0});
                });
  auto [result] = ex::sync_wait(sender).value();
  ASSERT_EQ(result, 2);
}

TEST(SchedulerTest, WeakPriorityThreadPoolLocalSlotEvictionTest) {
  ex_actor::WeakPriorityThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  std::vector<int> execution_order;
  stdexec::simple_counting_scope scope;

  auto sender = ex::schedule(scheduler) | ex::then([&]() {
                  stdexec::spawn(ex::schedule(scheduler) | ex::then([&]() { execution_order.push_back(2); }) |
                                     ex::write_env(ex::prop {ex_actor::get_priority, 2}) | ex_actor::DiscardResult(),
                                 scope.get_token());

                  stdexec::spawn(ex::schedule(scheduler) | ex::then([&]() { execution_order.push_back(1); }) |
                                     ex::write_env(ex::prop {ex_actor::get_priority, 1}) | ex_actor::DiscardResult(),
                                 scope.get_token());

                  stdexec::spawn(ex::schedule(scheduler) | ex::then([&]() { execution_order.push_back(3); }) |
                                     ex::write_env(ex::prop {ex_actor::get_priority, 3}) | ex_actor::DiscardResult(),
                                 scope.get_token());
                }) |
                ex::write_env(ex::prop {ex_actor::get_priority, 2});

  ex::sync_wait(sender);
  ex::sync_wait(scope.join());

  std::vector<int> expected = {1, 2, 3};
  ASSERT_EQ(execution_order, expected);
}

TEST(SchedulerTest, SchedulerUnionWithCorePinnedThreadPoolTest) {
  ex_actor::WorkSharingThreadPool ws_pool(1);
  ex_actor::CorePinnedThreadPool cp_pool(2);
  ex_actor::SchedulerUnion scheduler_union(ws_pool.GetScheduler(), cp_pool.GetScheduler());
  auto scheduler = scheduler_union.GetScheduler();
  auto start = ex::schedule(scheduler) | ex::then([] { return std::this_thread::get_id(); });

  auto sender1 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 0});
  auto sender2 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 1}) |
                 ex::write_env(ex::prop {ex_actor::get_core_index, 1});

  auto [thread_id1] = ex::sync_wait(sender1).value();
  auto [thread_id2] = ex::sync_wait(sender2).value();
  ASSERT_NE(thread_id1, thread_id2);

  auto coroutine = [&]() -> stdexec::task<void> {
    auto actor1 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.scheduler_index = 0});
    auto actor2 = co_await ex_actor::Spawn<TestActor2>().WithConfig({.scheduler_index = 1, .core_index = 1});

    uint64_t thread_id1 = co_await actor1.Send<&TestActor2::GetThreadId>();
    uint64_t thread_id2 = co_await actor2.Send<&TestActor2::GetThreadId>();
    EXPECT_NE(thread_id1, thread_id2);
  };
  ex_actor::Init(scheduler_union.GetScheduler());
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
