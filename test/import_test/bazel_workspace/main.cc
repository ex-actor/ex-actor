#include <iostream>

#include "ex_actor/api.h"

class Counter {
 public:
  int Add(int x) { return count_ += x; }

  // Tell me your methods - it's all you need to make your class an actor.
  constexpr static auto kActorMethods = std::make_tuple(&Counter::Add);

 private:
  int count_ = 0;
};

exec::task<void> TestBasicUseCase() {
  ex_actor::ActorRegistry registry;

  // Use any std::execution scheduler you like!
  ex_actor::WorkSharingThreadPool thread_pool(10);
  ex_actor::ActorRef counter = registry.CreateActor<Counter>(thread_pool.GetScheduler());

  // Coroutine support!
  std::cout << co_await counter.Call<&Counter::Add>(1) << '\n';
}

int main() {
  stdexec::sync_wait(TestBasicUseCase());
  return 0;
}