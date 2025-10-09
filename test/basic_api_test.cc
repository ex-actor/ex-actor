#include <cassert>
#include <iostream>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <exec/task.hpp>

#include "ex_actor/api.h"
#include "ex_actor/detail/actor_creation.h"

namespace ex = stdexec;

class Counter {
 public:
  void Add(int x) { count_ += x; }
  int GetValue() const { return count_; }

  void Error() const { throw std::runtime_error("error" + std::to_string(count_)); }

  constexpr static auto kActorMethods = std::make_tuple(&Counter::Add, &Counter::GetValue, &Counter::Error);

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

template <>
constexpr auto ex_actor::reflect::kActorMethods<Proxy> = std::make_tuple(&Proxy::GetValue, &Proxy::GetValue2);

static ex_actor::WorkSharingThreadPool thread_pool(10);

SCENARIO("exception in actor method should be propagated to the caller") {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::ActorRegistry registry;
    auto counter = registry.CreateActor<Counter>(thread_pool.GetScheduler());
    co_await counter.Send<&Counter::Error>();
  };
  REQUIRE_THROWS_WITH(ex::sync_wait(coroutine()), "error0");
}

SCENARIO("nest actor ref case") {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::ActorRegistry registry;

    ex_actor::ActorRef counter = registry.CreateActor<Counter>(thread_pool.GetScheduler());
    exec::async_scope scope;
    for (int i = 0; i < 100; ++i) {
      scope.spawn(counter.Send<&Counter::Add>(1));
    }
    auto res = co_await counter.Send<&Counter::GetValue>();
    REQUIRE(res == 100);

    ex_actor::ActorRef proxy = registry.CreateActor<Proxy>(thread_pool.GetScheduler(), counter);
    auto res2 = co_await proxy.Send<&Proxy::GetValue>();
    auto res3 = co_await proxy.Send<&Proxy::GetValue2>();
    REQUIRE(res2 == 100);
    REQUIRE(res3 == 100);
    co_return;
  };
  ex::sync_wait(coroutine());
}

SCENARIO("create actor with full config") {
  ex_actor::ActorRegistry registry;
  auto counter = registry.CreateActor<Counter>(
      ex_actor::ActorConfig {.scheduler = thread_pool.GetScheduler(), .max_message_executed_per_activation = 10});
  registry.CreateActor<Counter>(
      ex_actor::ActorConfig {.scheduler = thread_pool.GetScheduler(), .actor_name = "counter"});

  registry.CreateActor<Proxy>(
      ex_actor::ActorConfig {.scheduler = thread_pool.GetScheduler(), .mailbox_partition_size = 3}, counter);
  registry.GetActorByName<Counter>("counter");
  registry.DestroyActor(counter);
}