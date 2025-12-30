#include <cassert>
#include <iostream>
#include <stdexcept>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_registry.h"
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

TEST(BasicApiTest, ActorRegistryCreationWithDefaultScheduler) {
  auto coroutine = []() -> exec::task<void> {
    auto counter = co_await ex_actor::Spawn<Counter>();
    auto getvalue_sender = counter.Send<&Counter::GetValue>();
    auto getvalue_reply = co_await std::move(getvalue_sender);
    EXPECT_EQ(getvalue_reply, 0);
    co_return;
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(BasicApiTest, ShouldWorkWithAsyncSpawn) {
  auto coroutine = []() -> exec::task<void> {
    auto counter = co_await ex_actor::Spawn<Counter>();
    exec::async_scope scope;
    scope.spawn(counter.SendLocal<&Counter::Add>(1));
    auto future = scope.spawn_future(counter.SendLocal<&Counter::GetValue>());
    auto res = co_await std::move(future);
    EXPECT_EQ(res, 1);
    // Wait for all spawned work to complete before destroying the scope
    co_await scope.on_empty();
    co_return;
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(BasicApiTest, ExceptionInActorMethodShouldBePropagatedToCaller) {
  auto coroutine = []() -> exec::task<void> {
    auto counter = co_await ex_actor::Spawn<Counter>();
    co_await counter.Send<&Counter::Error>();
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ASSERT_THAT([&coroutine] { ex::sync_wait(coroutine()); },
              Throws<std::exception>(Property(&std::exception::what, HasSubstr("error0"))));
  ex_actor::Shutdown();
}

TEST(BasicApiTest, NestActorRefCase) {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::ActorRef counter = co_await ex_actor::Spawn<Counter>();
    exec::async_scope scope;
    for (int i = 0; i < 100; ++i) {
      scope.spawn(counter.Send<&Counter::Add>(1));
    }
    auto res = co_await counter.Send<&Counter::GetValue>();
    EXPECT_EQ(res, 100);

    ex_actor::ActorRef proxy = co_await ex_actor::Spawn<Proxy>(counter);
    auto res2 = co_await proxy.Send<&Proxy::GetValue>();
    auto res3 = co_await proxy.Send<&Proxy::GetValue2>();
    EXPECT_EQ(res2, 100);
    EXPECT_EQ(res3, 100);
    co_return;
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(BasicApiTest, CreateActorWithFullConfig) {
  auto coroutine = []() -> exec::task<void> {
    /*
    before gcc 13, we can't use heap-allocated temp variable after co_await, or there will be a double free error.
    here actor_name is heap allocated. so when using ActorConfig with actor_name, we should define it explicitly.

    i.e. you can't `co_await CreateActor<X>(ActorConfig {.actor_name = "A"})`, instead, you should do this:
    ```cpp
    ex_actor::ActorConfig a_config {.actor_name = "A"};
    auto remote_a = co_await registry.CreateActor<A, &A::Create>(a_config);
    ```

    see https://gcc.gnu.org/pipermail/gcc-bugs/2022-October/800402.html
    */
    ex_actor::ActorConfig config1 {.max_message_executed_per_activation = 10, .actor_name = "counter1"};
    auto counter = co_await ex_actor::Spawn<Counter>(config1);

    ex_actor::ActorConfig config2 {.actor_name = "counter2"};
    co_await ex_actor::Spawn<Counter>(config2);

    co_await ex_actor::Spawn<Counter>(ex_actor::ActorConfig {.scheduler_index = 0, .priority = 1});

    static_assert(rfl::internal::has_read_reflector<ex_actor::ActorRef<Counter>>);
    static_assert(rfl::internal::has_write_reflector<ex_actor::ActorRef<Counter>>);
    // test pass by lvalue
    ex_actor::ActorConfig config = {.max_message_executed_per_activation = 10};
    co_await ex_actor::Spawn<Proxy>(config, counter);
    co_await ex_actor::DestroyActor(counter);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

class TestActorWithNamedLookup {
 public:
  exec::task<ex_actor::ActorRef<Counter>> LookUpActor() {
    co_return (co_await ex_actor::GetActorRefByName<Counter>("counter")).value();
  }
};

TEST(BasicApiTest, LookUpNamedActor) {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::ActorConfig config {.actor_name = "counter"};
    co_await ex_actor::Spawn<Counter>(config);
    auto test_retriever_actor = co_await ex_actor::Spawn<TestActorWithNamedLookup>();

    auto lookup_sender = test_retriever_actor.Send<&TestActorWithNamedLookup::LookUpActor>();
    auto lookup_reply = co_await std::move(lookup_sender);

    auto actor = lookup_reply;
    auto getvalue_sender = actor.Send<&Counter::GetValue>();
    auto getvalue_reply = co_await std::move(getvalue_sender);
    EXPECT_EQ(getvalue_reply, 0);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
