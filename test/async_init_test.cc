#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = ex_actor::ex;

struct ConstructorThreadRecorder {
  explicit ConstructorThreadRecorder(std::thread::id* out) { *out = std::this_thread::get_id(); }
  void Noop() {}
};

TEST(AsyncInitTest, ConstructorRunsOnCorrectSchedulerWithEnv) {
  ex_actor::WorkSharingThreadPool thread_pool1(1);
  ex_actor::WorkSharingThreadPool thread_pool2(1);
  ex_actor::SchedulerUnion scheduler_union(thread_pool1.GetScheduler(), thread_pool2.GetScheduler());

  // Get the thread ID that runs on each pool.
  auto get_tid = ex::schedule(scheduler_union.GetScheduler()) | ex::then([] { return std::this_thread::get_id(); });
  auto [tid_pool1] = ex::sync_wait(get_tid | ex::write_env(ex::prop{ex_actor::get_scheduler_index, 0})).value();
  auto [tid_pool2] = ex::sync_wait(get_tid | ex::write_env(ex::prop{ex_actor::get_scheduler_index, 1})).value();
  ASSERT_NE(tid_pool1, tid_pool2);

  ex_actor::Init(scheduler_union.GetScheduler());
  auto coroutine = [&]() -> stdexec::task<void> {
    std::thread::id ctor_thread;
    auto actor = co_await ex_actor::Spawn<ConstructorThreadRecorder>(&ctor_thread)
                     .WithConfig({.scheduler_index = 1});
    // Constructor should have run on thread pool 2, not thread pool 1.
    EXPECT_EQ(ctor_thread, tid_pool2);
    EXPECT_NE(ctor_thread, tid_pool1);
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

struct SlowActor {
  explicit SlowActor(std::atomic_int* concurrent_count, std::atomic_int* max_concurrent) {
    int cur = concurrent_count->fetch_add(1, std::memory_order_relaxed) + 1;
    int prev_max = max_concurrent->load(std::memory_order_relaxed);
    while (cur > prev_max && !max_concurrent->compare_exchange_weak(prev_max, cur)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    concurrent_count->fetch_sub(1, std::memory_order_relaxed);
  }
  void Noop() {}
};

TEST(AsyncInitTest, ConstructorsRunInParallel) {
  ex_actor::Init(/*thread_pool_size=*/4);
  auto coroutine = []() -> stdexec::task<void> {
    std::atomic_int concurrent_count = 0;
    std::atomic_int max_concurrent = 0;

    stdexec::simple_counting_scope scope;
    constexpr int kNumActors = 4;

    for (int i = 0; i < kNumActors; ++i) {
      stdexec::spawn(
          ex_actor::Spawn<SlowActor>(&concurrent_count, &max_concurrent) | ex_actor::DiscardResult(),
          scope.get_token());
    }
    co_await scope.join();

    // If constructors ran in parallel, max_concurrent should be > 1.
    EXPECT_GT(max_concurrent.load(), 1);
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
