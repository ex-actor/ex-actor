#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

TEST(UtilTest, SemaphoreBasicUsageCase) {
  ex_actor::util::Semaphore semaphore(0);
  ex_actor::ex::sync_wait(semaphore.OnDrained());
  ASSERT_EQ(semaphore.Release(10), 10);
  std::jthread t([&semaphore]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_EQ(semaphore.Acquire(11), -1);
  });
  ex_actor::ex::sync_wait(semaphore.OnDrained());
}

// Stress test to catch the data race between Acquire() and OnDrained().
// The race occurs when:
//   Thread T1 (Acquire)         Thread T2 (OnDrained::start)
//   ─────────────────────       ───────────────────────────
//   fetch_sub: 1→0
//                               lock mutex
//                               load count_ (sees 0!)
//                               unlock
//                               Complete() → may destroy scope
//   lock mutex
//   → USE-AFTER-FREE
//
// The fix uses pending_notifiers_ counter to prevent OnDrained from completing
// while an Acquire() is in-flight but hasn't locked the mutex yet.
TEST(UtilTest, SemaphoreAcquireOnDrainedRaceCondition) {
  constexpr int kIterations = 1000;
  constexpr int kThreads = 8;

  for (int iter = 0; iter < kIterations; ++iter) {
    auto semaphore = std::make_unique<ex_actor::util::Semaphore>(kThreads);
    std::atomic<bool> start_flag {false};
    std::atomic<int> completed_acquires {0};

    std::vector<std::jthread> threads;
    threads.reserve(kThreads);

    // Spawn threads that will all call Acquire(1) simultaneously
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&semaphore, &start_flag, &completed_acquires]() {
        // Spin until start flag is set to maximize contention
        while (!start_flag.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        semaphore->Acquire(1);
        completed_acquires.fetch_add(1, std::memory_order_relaxed);
      });
    }

    // Start a thread that waits for OnDrained and then destroys the semaphore
    std::jthread waiter([&semaphore, &start_flag]() {
      while (!start_flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      ex_actor::ex::sync_wait(semaphore->OnDrained());
      // After OnDrained completes, destroy the semaphore.
      // If there's a race, an Acquire() thread may still try to access it.
      semaphore.reset();
    });

    // Release all threads simultaneously to maximize race window
    start_flag.store(true, std::memory_order_release);

    // Wait for waiter to complete (which destroys semaphore)
    waiter.join();

    // Wait for all acquire threads to complete
    threads.clear();

    // Verify all acquires completed
    ASSERT_EQ(completed_acquires.load(), kThreads);
  }
}