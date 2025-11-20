#include <cassert>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_creation.h"
#include "rfl/internal/has_reflector.hpp"

namespace ex = stdexec;

using testing::HasSubstr;
using testing::Property;
using testing::Throws;

class Counter {
 public:
  void Add(int x) { count_ += x; }
  int GetValue() const { return count_; }

  void Error() const { throw std::runtime_error("error" + std::to_string(count_)); }

 private:
  int count_ = 0;
};

class Proxy {
 public:
  explicit Proxy(ex_actor::ActorRef<Counter> actor_ref) : actor_ref_(actor_ref) {}

  exec::task<int> GetValue() {
    int res = co_await actor_ref_.template Send<&Counter::GetValue>();
    std::cout << "This line runs on the current actor(Proxy), because coroutine has scheduler affinity\n";
    co_return res;
  }

  ex::sender auto GetValue2() {
    // TODO find a way to get parent scheduler, so we can call continues_on
    return actor_ref_.template Send<&Counter::GetValue>() | ex::then([](int value) {
             std::cout << "This line runs on the target actor(Counter), unless you call continue_on explicitly.\n";
             return value;
           });
  }

 private:
  ex_actor::ActorRef<Counter> actor_ref_;
};

class TestActorWithNamedLookup {
 public:
  explicit TestActorWithNamedLookup(std::weak_ptr<ex_actor::ActorRegistry> reg) : registry_(std::move(reg)) {}
  auto LookUpActor() {
    if (registry_.expired()) throw std::runtime_error("Registry pointer expired before we could look up the actor!");
    auto ptr = registry_.lock();
    return ptr->GetActorRefByName<Counter>("counter");
  }

 private:
  std::weak_ptr<ex_actor::ActorRegistry> registry_;
};

TEST(BasicApiTest, ActorRegistryCreationWithDefaultScheduler) {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/10);
  auto counter = registry.CreateActor<Counter>();
  auto getvalue_sender = counter.Send<&Counter::GetValue>();
  auto getvalue_reply = ex::sync_wait(std::move(getvalue_sender));
  ASSERT_EQ(getvalue_reply.has_value(), true);
  auto [value] = getvalue_reply.value();
  ASSERT_EQ(value, 0);
}

TEST(BasicApiTest, ShouldWorkWithAsyncSpawn) {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);
  auto counter = registry.CreateActor<Counter>();
  exec::async_scope scope;
  scope.spawn(counter.Send<&Counter::Add>(1));
  auto future = scope.spawn_future(counter.Send<&Counter::GetValue>());
  auto [res] = stdexec::sync_wait(std::move(future)).value();
  ASSERT_EQ(res, 1);
}

TEST(BasicApiTest, ExceptionInActorMethodShouldBePropagatedToCaller) {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::WorkSharingThreadPool thread_pool(10);
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
    auto counter = registry.CreateActor<Counter>();
    co_await counter.Send<&Counter::Error>();
  };
  ASSERT_THAT([&coroutine] { ex::sync_wait(coroutine()); },
              Throws<std::exception>(Property(&std::exception::what, HasSubstr("error0"))));
}

TEST(BasicApiTest, NestActorRefCase) {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::WorkSharingThreadPool thread_pool(10);
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
    ex_actor::ActorRef counter = registry.CreateActor<Counter>();
    exec::async_scope scope;
    for (int i = 0; i < 100; ++i) {
      scope.spawn(counter.Send<&Counter::Add>(1));
    }
    auto res = co_await counter.Send<&Counter::GetValue>();
    EXPECT_EQ(res, 100);

    ex_actor::ActorRef proxy = registry.CreateActor<Proxy>(counter);
    auto res2 = co_await proxy.Send<&Proxy::GetValue>();
    auto res3 = co_await proxy.Send<&Proxy::GetValue2>();
    EXPECT_EQ(res2, 100);
    EXPECT_EQ(res3, 100);
    co_return;
  };
  ex::sync_wait(coroutine());
}

TEST(BasicApiTest, CreateActorWithFullConfig) {
  ex_actor::WorkSharingThreadPool thread_pool(10);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());
  auto counter = registry.CreateActor<Counter>(
      ex_actor::ActorConfig {.max_message_executed_per_activation = 10, .actor_name = "counter1"});
  registry.CreateActor<Counter>(ex_actor::ActorConfig {.actor_name = "counter2"});
  registry.CreateActor<Counter>(ex_actor::ActorConfig {.scheduler_index = 0, .priority = 1});

  static_assert(rfl::internal::has_read_reflector<ex_actor::ActorRef<Counter>>);
  static_assert(rfl::internal::has_write_reflector<ex_actor::ActorRef<Counter>>);
  // test pass by lvalue
  ex_actor::ActorConfig config = {.max_message_executed_per_activation = 10};
  registry.CreateActor<Proxy>(config, counter);
  registry.DestroyActor(counter);
}

TEST(BasicApiTest, LookUpNamedActor) {
  ex_actor::WorkSharingThreadPool thread_pool(10);
  auto registry_ptr = std::make_shared<ex_actor::ActorRegistry>(thread_pool.GetScheduler());
  registry_ptr->CreateActor<Counter>(ex_actor::ActorConfig {.actor_name = "counter"});
  auto test_retriever_actor = registry_ptr->CreateActor<TestActorWithNamedLookup>(registry_ptr);

  auto lookup_sender = test_retriever_actor.Send<&TestActorWithNamedLookup::LookUpActor>();
  auto lookup_reply = ex::sync_wait(std::move(lookup_sender));
  ASSERT_EQ(lookup_reply.has_value(), true);
  auto [lookup_result] = lookup_reply.value();
  ASSERT_EQ(lookup_result.has_value(), true);

  auto actor = lookup_result.value();
  auto getvalue_sender = actor.Send<&Counter::GetValue>();
  auto getvalue_reply = ex::sync_wait(std::move(getvalue_sender));
  ASSERT_EQ(getvalue_reply.has_value(), true);
  auto [value] = getvalue_reply.value();
  ASSERT_EQ(value, 0);
}
