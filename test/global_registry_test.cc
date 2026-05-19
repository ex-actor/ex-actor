#include <csignal>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

// --- GetNodeId tests ---

TEST(GlobalRegistryTest, GetNodeIdReturnsNonZeroAfterInit) {
  ex_actor::Init(/*thread_pool_size=*/2);
  uint64_t node_id = ex_actor::GetNodeId();
  EXPECT_NE(node_id, 0u);
  ex_actor::Shutdown();
}

TEST(GlobalRegistryTest, GetNodeIdHasHighBitClear) {
  // Cap'n Proto requires serializable int64, so the sign bit must be zero
  ex_actor::Init(/*thread_pool_size=*/2);
  uint64_t node_id = ex_actor::GetNodeId();
  EXPECT_EQ(node_id & 0x8000'0000'0000'0000, 0u);
  ex_actor::Shutdown();
}

TEST(GlobalRegistryTest, GetNodeIdIsStableWithinSession) {
  ex_actor::Init(/*thread_pool_size=*/2);
  uint64_t id1 = ex_actor::GetNodeId();
  uint64_t id2 = ex_actor::GetNodeId();
  EXPECT_EQ(id1, id2);
  ex_actor::Shutdown();
}

TEST(GlobalRegistryTest, GetNodeIdThrowsWhenNotInitialized) {
  EXPECT_ANY_THROW(ex_actor::GetNodeId());
}

// --- WaitOsExitSignal tests ---

TEST(GlobalRegistryTest, WaitOsExitSignalCompletesOnSIGINT) {
  ex_actor::Init(/*thread_pool_size=*/2);

  auto coroutine = []() -> stdexec::task<void> {
    std::thread signal_thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::raise(SIGINT);
    });

    co_await ex_actor::WaitOsExitSignal();
    signal_thread.join();
  };

  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(GlobalRegistryTest, WaitOsExitSignalCompletesOnSIGTERM) {
  ex_actor::Init(/*thread_pool_size=*/2);

  auto coroutine = []() -> stdexec::task<void> {
    std::thread signal_thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::raise(SIGTERM);
    });

    co_await ex_actor::WaitOsExitSignal();
    signal_thread.join();
  };

  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(GlobalRegistryTest, WaitOsExitSignalRestoresOriginalHandler) {
  ex_actor::Init(/*thread_pool_size=*/2);

  static volatile sig_atomic_t custom_handler_called;
  custom_handler_called = 0;
  auto custom_handler = [](int) { custom_handler_called = 1; };
  std::signal(SIGINT, custom_handler);

  auto coroutine = []() -> stdexec::task<void> {
    std::thread signal_thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::raise(SIGINT);
    });

    co_await ex_actor::WaitOsExitSignal();
    signal_thread.join();
  };

  ex::sync_wait(coroutine());

  // After WaitOsExitSignal completes, the original handler should be restored
  auto current_handler = std::signal(SIGINT, SIG_DFL);
  EXPECT_EQ(current_handler, custom_handler);

  // Restore default for subsequent tests
  std::signal(SIGINT, SIG_DFL);
  ex_actor::Shutdown();
}

TEST(GlobalRegistryTest, WaitOsExitSignalInvokesPreviousHandler) {
  ex_actor::Init(/*thread_pool_size=*/2);

  static volatile sig_atomic_t prev_handler_invoked;
  prev_handler_invoked = 0;
  auto prev_handler = [](int) { prev_handler_invoked = 1; };
  std::signal(SIGINT, prev_handler);

  auto coroutine = []() -> stdexec::task<void> {
    std::thread signal_thread([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::raise(SIGINT);
    });

    co_await ex_actor::WaitOsExitSignal();
    signal_thread.join();
  };

  ex::sync_wait(coroutine());

  EXPECT_EQ(prev_handler_invoked, 1);

  std::signal(SIGINT, SIG_DFL);
  ex_actor::Shutdown();
}
