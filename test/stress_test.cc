#include <chrono>

#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/scheduler.h"

struct TestActor {
  void Run() {
    if (running) {
      throw std::runtime_error("Running in multiple threads");
    }
    running = true;
    std::this_thread::sleep_for(std::chrono::microseconds(1));
    running = false;
  }

  constexpr static auto kActorMethods = std::make_tuple(&TestActor::Run);

  bool running = false;
};

TEST(StressTest, ActorShouldOnlyBeExecutedInOneThread) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
  auto [actor] = stdexec::sync_wait(registry.CreateActor<TestActor>()).value();
  std::vector<std::jthread> threads;
  threads.reserve(10);
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([actor]() {
      for (int j = 0; j < 10; ++j) {
        ex_actor::ex::sync_wait(actor.Send<&TestActor::Run>());
      }
    });
  }
}