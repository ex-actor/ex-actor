#include <atomic>
#include <chrono>
#include <thread>

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
  auto [actor] = stdexec::sync_wait(registry.CreateActor<TestActor>()).value();
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
  exec::async_scope scope;
  scope.spawn(sender1);
  scope.spawn(sender2);
  scope.spawn(sender3);
  thread_pool.StartWorkers();
  ex::sync_wait(scope.on_empty());
  ASSERT_EQ(count, 3);
}

TEST(SchedulerTest, SchedulerUnionTest) {
  ex_actor::WorkSharingThreadPool thread_pool1(1);
  ex_actor::WorkSharingThreadPool thread_pool2(1);
  ex_actor::SchedulerUnion scheduler_union(std::vector<ex_actor::WorkSharingThreadPool::Scheduler> {
      thread_pool1.GetScheduler(), thread_pool2.GetScheduler()});
  auto scheduler = scheduler_union.GetScheduler();
  auto start = ex::schedule(scheduler) | ex::then([] { return std::this_thread::get_id(); });

  auto sender1 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 0});
  auto sender2 = start | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 1});
  auto [thread_id1] = ex::sync_wait(sender1).value();
  auto [thread_id2] = ex::sync_wait(sender2).value();
  ASSERT_NE(thread_id1, thread_id2);
}