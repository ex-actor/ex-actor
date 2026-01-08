#include <atomic>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace logging = ex_actor::internal::logging;

class Counter {
 public:
  void Increment() { semaphore_.Acquire(1); }
  uint32_t GetCount() const { return 10 - semaphore_.CurrentValue(); }

  exec::task<void> WaitAllMessageArrived() { co_await semaphore_.OnDrained(); }

 private:
  ex_actor::util::Semaphore semaphore_ {10};
};

class ManualActivationActor {
 public:
  explicit ManualActivationActor(ex_actor::ActorRef<Counter> counter) : counter_(counter) {}
  bool ExActorOnUnsafeMessageSlotFilled(size_t /*unsafe_message_slot_index*/) {
    size_t cnt = unsafe_message_cnt_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (cnt == 10) {
      logging::Info("activate the actor");
      return true;
    }
    return false;
  }

  void Ping() { async_scope_.spawn(counter_.Send<&Counter::Increment>()); }

 private:
  std::atomic_size_t unsafe_message_cnt_ = 0;
  exec::async_scope async_scope_;
  ex_actor::ActorRef<Counter> counter_;
};

TEST(ManualActivationTest, ManualActivation) {
  ex_actor::Init(/*thread_pool_size=*/1);

  auto main_coro = []() -> exec::task<void> {
    exec::async_scope async_scope;
    auto counter = co_await ex_actor::Spawn<Counter>();

    // 10 mailboxes
    ex_actor::ActorConfig config = {.unsafe_message_slots = 10};
    auto manual_activation_actor = co_await ex_actor::Spawn<ManualActivationActor>(config, counter);

    logging::Info("Sending to unsafe slot 0 to 8, should not activate the actor");
    for (int i = 0; i < 9; i++) {
      async_scope.spawn(manual_activation_actor.UnsafeMessageSlot(i).Send<&ManualActivationActor::Ping>());
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    logging::Info("Checking count, should be 0");
    uint32_t count = co_await counter.Send<&Counter::GetCount>();
    EXPECT_EQ(count, 0);

    logging::Info("Sending to unsafe slot 9, now has 10 messages, should activate the actor");
    co_await manual_activation_actor.UnsafeMessageSlot(9).Send<&ManualActivationActor::Ping>();

    co_await counter.Send<&Counter::WaitAllMessageArrived>();
    count = co_await counter.Send<&Counter::GetCount>();

    co_await async_scope.on_empty();
  };

  stdexec::sync_wait(main_coro());

  ex_actor::Shutdown();
}