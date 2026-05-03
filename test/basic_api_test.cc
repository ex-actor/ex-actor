#include <atomic>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <tuple>

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

  stdexec::task<int> GetValue() {
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
  auto coroutine = []() -> stdexec::task<void> {
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
  auto coroutine = []() -> stdexec::task<void> {
    auto counter = co_await ex_actor::Spawn<Counter>();
    stdexec::simple_counting_scope scope;
    stdexec::spawn(counter.SendLocal<&Counter::Add>(1) | ex_actor::DiscardResult(), scope.get_token());
    auto future = stdexec::spawn_future(counter.SendLocal<&Counter::GetValue>(), scope.get_token());
    auto res = co_await std::move(future);
    EXPECT_EQ(res, 1);
    co_await scope.join();
    co_return;
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(BasicApiTest, ExceptionInActorMethodShouldBePropagatedToCaller) {
  auto coroutine = []() -> stdexec::task<void> {
    auto counter = co_await ex_actor::Spawn<Counter>();
    co_await counter.Send<&Counter::Error>();
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ASSERT_THAT([&coroutine] { ex::sync_wait(coroutine()); },
              Throws<std::exception>(Property(&std::exception::what, HasSubstr("error0"))));
  ex_actor::Shutdown();
}

TEST(BasicApiTest, NestActorRefCase) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorRef counter = co_await ex_actor::Spawn<Counter>();
    stdexec::simple_counting_scope scope;
    for (int i = 0; i < 100; ++i) {
      stdexec::spawn(counter.Send<&Counter::Add>(1) | ex_actor::DiscardResult(), scope.get_token());
    }
    auto res = co_await counter.Send<&Counter::GetValue>();
    EXPECT_EQ(res, 100);

    ex_actor::ActorRef proxy = co_await ex_actor::Spawn<Proxy>(counter);
    auto res2 = co_await proxy.Send<&Proxy::GetValue>();
    auto res3 = co_await proxy.Send<&Proxy::GetValue2>();
    EXPECT_EQ(res2, 100);
    EXPECT_EQ(res3, 100);
    co_await scope.join();
    co_return;
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(BasicApiTest, SpawnWithFullConfig) {
  auto coroutine = []() -> stdexec::task<void> {
    // GCC < 13 workaround: assign configs with heap-allocated fields to named variables before co_await.
    // See docs/contents/installation.md "Known Issues: GCC before 13".
    ex_actor::ActorConfig config1 {.max_message_executed_per_activation = 10, .actor_name = "counter1"};
    auto counter = co_await ex_actor::Spawn<Counter>().WithConfig(config1);

    ex_actor::ActorConfig config2 {.actor_name = "counter2"};
    co_await ex_actor::Spawn<Counter>().WithConfig(config2);

    co_await ex_actor::Spawn<Counter>().WithConfig({.scheduler_index = 0, .priority = 1});

    static_assert(rfl::internal::has_read_reflector<ex_actor::ActorRef<Counter>>);
    static_assert(rfl::internal::has_write_reflector<ex_actor::ActorRef<Counter>>);
    // test pass by lvalue
    ex_actor::ActorConfig config = {.max_message_executed_per_activation = 10};
    co_await ex_actor::Spawn<Proxy>(counter).WithConfig(config);
    co_await ex_actor::DestroyActor(counter);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

class TestActorWithNamedLookup {
 public:
  stdexec::task<ex_actor::ActorRef<Counter>> LookUpActor() {
    co_return (co_await ex_actor::GetActorRefByName<Counter>("counter")).value();
  }
};

TEST(BasicApiTest, LookUpNamedActor) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.actor_name = "counter"};
    co_await ex_actor::Spawn<Counter>().WithConfig(config);
    auto test_retriever_actor = co_await ex_actor::Spawn<TestActorWithNamedLookup>();

    auto lookup_sender = test_retriever_actor.Send<&TestActorWithNamedLookup::LookUpActor>();
    auto lookup_reply = co_await std::move(lookup_sender);

    const auto& actor = lookup_reply;
    auto getvalue_sender = actor.Send<&Counter::GetValue>();
    auto getvalue_reply = co_await std::move(getvalue_sender);
    EXPECT_EQ(getvalue_reply, 0);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(BasicApiTest, RemoteActorRefSlicedToBasicActorRefShouldThrowOnUse) {
  ex_actor::internal::ActorRef<Counter> remote_ref(
      /*this_node_id=*/1, /*node_id=*/2, /*actor_id=*/42, /*actor=*/nullptr, /*broker_actor_ref=*/ {});
  // Slicing a remote ActorRef to BasicActorRef is allowed at the type level (due to inheritance),
  // but calling SendLocal on it will throw because type_erased_actor_ is nullptr.
  ex_actor::BasicActorRef<Counter> local_ref = remote_ref;
  EXPECT_THAT([&] { std::ignore = local_ref.SendLocal<&Counter::GetValue>(); },
              Throws<std::exception>(Property(&std::exception::what, HasSubstr("Underlying actor instance not set"))));
}

struct Base {
  virtual ~Base() = default;
  virtual std::string Foo() = 0;
};

struct Derived : Base {
  std::string Foo() override { return "Derived::Foo"; }
};

TEST(BasicApiTest, ActorCanBePolymorphic) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::Init(/*thread_pool_size=*/10);
    ex_actor::ActorRef<Base> base = co_await ex_actor::Spawn<Derived>();
    std::string foo_reply = co_await base.Send<&Base::Foo>();
    EXPECT_EQ(foo_reply, "Derived::Foo");
    ex_actor::Shutdown();
  };
  ex::sync_wait(coroutine());
}

struct SlowPendingActor {
  void Wait(int ms, std::atomic<int>* counter) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    (*counter)++;
  }
};

TEST(BasicApiTest, GetPendingMessageCountShouldWork) {
  std::atomic<int> counter {0};
  auto coroutine = [&]() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<SlowPendingActor>();

    // 1. Send 5 long-running tasks
    stdexec::simple_counting_scope scope;
    for (int i = 0; i < 5; ++i) {
      stdexec::spawn(actor.SendLocal<&SlowPendingActor::Wait>(200, &counter) | ex_actor::DiscardResult(),
                     scope.get_token());
    }

    // 2. Give it a tiny bit of time to start the first one
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 3. Check pending count using transparent API
    size_t pending = co_await actor.GetPendingMessageCount();
    EXPECT_GE(pending, 1U);
    EXPECT_LE(pending, 5U);

    // 4. Check pending count using sync API
    size_t pending_sync = actor.GetPendingMessageCountLocal();
    EXPECT_EQ(pending_sync, pending);

    std::cout << "Detected " << pending << " pending messages" << std::endl;

    co_await scope.join();
    EXPECT_EQ(actor.GetPendingMessageCountLocal(), 0U);
    EXPECT_EQ(counter.load(), 5);
  };

  ex_actor::Init(/*thread_pool_size=*/1);  // Use 1 thread to ensure queueing
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}