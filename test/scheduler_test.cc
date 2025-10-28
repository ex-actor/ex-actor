#include <atomic>
#include <chrono>

#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_config.h"

namespace ex = ex_actor::ex;

TEST(SchedulerTest, TaskInWorkSharingThreadPoolShouldBeStoppable) {
  ex_actor::WorkSharingThreadPool thread_pool(1);
  auto scheduler = thread_pool.GetScheduler();
  auto task = ex::schedule(scheduler) | ex::then([]() { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
  exec::async_scope scope;
  for (int i = 0; i < 100000; ++i) {
    scope.spawn(task);
  }
  scope.request_stop();
  ex::sync_wait(scope.on_empty());
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
  auto actor = registry.CreateActor<TestActor>();
  exec::async_scope scope;
  for (int i = 0; i < 100000; ++i) {
    scope.spawn(actor.Send<&TestActor::Foo>());
  }
  scope.request_stop();
  ex::sync_wait(scope.on_empty());
}

TEST(SchedulerTest, PrioritySchedulerTest) {
  ex_actor::PriorityThreadPool thread_pool(1, /*start_workers_immediately=*/false);
  auto scheduler = thread_pool.GetScheduler();
  std::atomic_int count = 0;
  auto sender1 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 0);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ex_actor::get_std_exec_env,
                                         std::unordered_map<std::string, std::string> {{"priority", "1"}}});
  auto sender2 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 2);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ex_actor::get_std_exec_env,
                                         std::unordered_map<std::string, std::string> {{"priority", "3"}}});
  auto sender3 = ex::schedule(scheduler) | ex::then([&count]() {
                   EXPECT_EQ(count, 1);
                   count++;
                 }) |
                 ex::write_env(ex::prop {ex_actor::get_std_exec_env,
                                         std::unordered_map<std::string, std::string> {{"priority", "2"}}});
  exec::async_scope scope;
  scope.spawn(sender1);
  scope.spawn(sender2);
  scope.spawn(sender3);
  thread_pool.StartWorkers();
  ex::sync_wait(scope.on_empty());
  ASSERT_EQ(count, 3);
}