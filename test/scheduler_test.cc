#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "ex_actor/api.h"

namespace ex = ex_actor::ex;

SCENARIO("task in WorkSharingThreadPool should be stoppable") {
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
SCENARIO("Actor task should be stoppable") {
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