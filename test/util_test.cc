#include <thread>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

TEST(UtilTest, SemaphoreBasicUsageCase) {
  ex_actor::Semaphore semaphore(0);
  ex_actor::ex::sync_wait(semaphore.OnDrained());
  ASSERT_EQ(semaphore.Release(10), 10);
  std::jthread t([&semaphore]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_EQ(semaphore.Acquire(11), -1);
  });
  ex_actor::ex::sync_wait(semaphore.OnDrained());
}