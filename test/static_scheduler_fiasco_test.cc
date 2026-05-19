#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = ex_actor::ex;

// For https://github.com/ex-actor/ex-actor/issues/265
// Reproduces the static destruction order fiasco: when thread_pool is a function-local static,
// global_manager (inside Create_global_manager) is destroyed before thread_pool. When
// thread_pool's workers exit, their ~MPSC_thread_hook() accesses the destroyed global_manager.
struct NoOpActor {
  void Ping() {}
};
TEST(SchedulerTest, TestStaticSchedulerFiasco) {
  static ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::Init(thread_pool.GetScheduler());

  // Spawn an actor and send messages so that worker threads in thread_pool actually
  // touch the MPSC queue (registering their thread-local MPSC_thread_hook).
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<NoOpActor>();
    for (int i = 0; i < 100; ++i) {
      co_await actor.Send<&NoOpActor::Ping>();
    }
  };
  ex::sync_wait(coroutine());

  ex_actor::Shutdown();
  // After Shutdown(), thread_pool's workers are still alive (Shutdown doesn't stop them).
  // At program exit, static destruction order:
  //   1. global_manager destroyed (constructed later, destroyed first)
  //   2. thread_pool destroyed -> workers exit -> ~MPSC_thread_hook() -> SIGSEGV
}
