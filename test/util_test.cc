#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "ex_actor/api.h"

SCENARIO("Semaphore basic usage case") {
  ex_actor::util::Semaphore semaphore(0);
  ex_actor::ex::sync_wait(semaphore.OnDrained());
  REQUIRE(semaphore.Release(10) == 10);
  std::jthread t([&semaphore]() {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    REQUIRE(semaphore.Acquire(11) == -1);
  });
  ex_actor::ex::sync_wait(semaphore.OnDrained());
}