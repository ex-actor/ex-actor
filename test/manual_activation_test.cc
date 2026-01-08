#include <gtest/gtest.h>

#include "ex_actor/api.h"

class Counter {
 public:
  void Increment() { count_++; }
  uint32_t GetCount() const { return count_; }

 private:
  uint32_t count_ = 0;
};

class ManualActivationActor {
 public:
  explicit ManualActivationActor(ex_actor::ActorRef<Counter> counter) : counter_(counter) {}
  bool ExActorManualActivationHook(ex_actor::MessagePushEvent event) {
    // only activate when message is pushed to mailbox 0
    return event.mailbox_index == 0 || event.is_destroy_message;
  }

  exec::task<void> Ping() { co_await counter_.Send<&Counter::Increment>(); }

 private:
  ex_actor::ActorRef<Counter> counter_;
};

TEST(ManualActivationTest, ManualActivation) {
  ex_actor::Init(/*thread_pool_size=*/1);

  auto main_coro = []() -> exec::task<void> {
    exec::async_scope async_scope;
    auto counter = co_await ex_actor::Spawn<Counter>();

    // 10 mailboxes
    ex_actor::ActorConfig config = {.mailbox_configs = {10, ex_actor::ActorConfig::OneSlotUnsafeMailboxConfig {}}};
    auto manual_activation_actor = co_await ex_actor::Spawn<ManualActivationActor>(config, counter);

    // send to mailbox 1 to 9, should not activate the actor
    for (int i = 1; i < 10; i++) {
      async_scope.spawn(manual_activation_actor.Mailbox(i).Send<&ManualActivationActor::Ping>());
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    uint32_t count = co_await counter.Send<&Counter::GetCount>();
    EXPECT_EQ(count, 0);

    // send to mailbox 0, should activate the actor
    co_await manual_activation_actor.Mailbox(0).Send<&ManualActivationActor::Ping>();
    count = co_await counter.Send<&Counter::GetCount>();
    EXPECT_EQ(count, 10);

    co_await async_scope.on_empty();
  };

  stdexec::sync_wait(main_coro());

  ex_actor::Shutdown();
}