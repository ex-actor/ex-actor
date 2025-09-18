#include <iostream>
#include <tuple>

#include <exec/task.hpp>

#include "ex_actor/api.h"

namespace ex = stdexec;

class Counter {
 public:
  void Add(int x) { count_ += x; }
  int GetValue() const { return count_; }

  constexpr static auto kActorMethods = std::make_tuple(&Counter::Add, &Counter::GetValue);

 private:
  int count_ = 0;
};

class Proxy {
 public:
  explicit Proxy(ex_actor::ActorRef<Counter> actor_ref) : actor_ref_(actor_ref) {}

  exec::task<int> GetValue() {
    int res = co_await actor_ref_.template Call<&Counter::GetValue>();
    std::cout << "This line runs on the current actor(Proxy), because coroutine has scheduler affinity\n";
    co_return res;
  }

  ex::sender auto GetValue2() {
    // TODO find a way to get parent scheduler, so we can call continues_on
    return actor_ref_.template Call<&Counter::GetValue>() | ex::then([](int value) {
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

static exec::task<void> TestBasicUsage() {
  ex_actor::ActorRegistry registry;

  ex_actor::ActorRef counter = registry.CreateActor<Counter>(thread_pool.GetScheduler());
  exec::async_scope scope;
  for (int i = 0; i < 100; ++i) {
    scope.spawn(counter.Call<&Counter::Add>(1));
  }
  auto res = co_await counter.Call<&Counter::GetValue>();
  std::cout << "after add1*100 : " << res << '\n';

  ex_actor::ActorRef proxy = registry.CreateActor<Proxy>(thread_pool.GetScheduler(), counter);
  auto res2 = co_await proxy.Call<&Proxy::GetValue>();
  auto res3 = co_await proxy.Call<&Proxy::GetValue2>();

  std::cout << "value through broker: " << res2 << "," << res3 << '\n';

  co_return;
}

static exec::task<void> TestConfig() {
  ex_actor::ActorRegistry registry;
  auto counter = registry.CreateActor<Counter>(ex_actor::ActorConfig {.scheduler = thread_pool.GetScheduler()});
  registry.CreateActor<Counter>(
      ex_actor::ActorConfig {.scheduler = thread_pool.GetScheduler(), .actor_name = "counter"});
  registry.CreateActor<Proxy>(ex_actor::ActorConfig {.scheduler = thread_pool.GetScheduler()}, counter);
  registry.GetActorByName<Counter>("counter");
  registry.DestroyActor(counter);
  co_return;
}

int main() {
  ex::sync_wait(TestBasicUsage());
  ex::sync_wait(TestConfig());
  return 0;
}
