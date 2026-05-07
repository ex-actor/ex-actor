#include <atomic>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

// ===========================
// Test Actor Classes
// ===========================

class MultiMailboxActor {
 public:
  void Append(const std::string& value) { log_.push_back(value); }

  std::vector<std::string> GetLog() const { return log_; }

  int Add(int x) {
    sum_ += x;
    return sum_;
  }

  int GetSum() const { return sum_; }

 private:
  std::vector<std::string> log_;
  int sum_ = 0;
};

class BoundedQueueActor {
 public:
  void Increment() { count_++; }

  int GetCount() const { return count_; }

  int Add(int x) {
    count_ += x;
    return count_;
  }

 private:
  int count_ = 0;
};

// ===========================
// Multiple Mailbox Tests
// ===========================

TEST(MailboxTest, MultipleMailboxesSendLocal) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(3, ex_actor::UnboundedThreadSafeMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    co_await actor.Mailbox(0).SendLocal<&MultiMailboxActor::Append>("from_mailbox_0");
    co_await actor.Mailbox(1).SendLocal<&MultiMailboxActor::Append>("from_mailbox_1");
    co_await actor.Mailbox(2).SendLocal<&MultiMailboxActor::Append>("from_mailbox_2");

    auto log = co_await actor.SendLocal<&MultiMailboxActor::GetLog>();
    EXPECT_EQ(log.size(), 3);
    EXPECT_EQ(log[0], "from_mailbox_0");
    EXPECT_EQ(log[1], "from_mailbox_1");
    EXPECT_EQ(log[2], "from_mailbox_2");
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, MultipleMailboxesSend) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(2, ex_actor::UnboundedThreadSafeMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    co_await actor.Mailbox(0).Send<&MultiMailboxActor::Append>("via_send_mb0");
    co_await actor.Mailbox(1).Send<&MultiMailboxActor::Append>("via_send_mb1");

    auto log = co_await actor.Send<&MultiMailboxActor::GetLog>();
    EXPECT_EQ(log.size(), 2);
    EXPECT_EQ(log[0], "via_send_mb0");
    EXPECT_EQ(log[1], "via_send_mb1");
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, DefaultMailboxIndexIsZero) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(2, ex_actor::UnboundedThreadSafeMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    co_await actor.Send<&MultiMailboxActor::Add>(10);
    int sum_via_default = co_await actor.Send<&MultiMailboxActor::GetSum>();
    EXPECT_EQ(sum_via_default, 10);

    co_await actor.Mailbox(0).Send<&MultiMailboxActor::Add>(5);
    int sum_via_mb0 = co_await actor.Send<&MultiMailboxActor::GetSum>();
    EXPECT_EQ(sum_via_mb0, 15);
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, MultipleMailboxesRoundRobinDraining) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(3, ex_actor::UnboundedThreadSafeMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    co_await actor.Mailbox(0).SendLocal<&MultiMailboxActor::Append>("a0");
    co_await actor.Mailbox(0).SendLocal<&MultiMailboxActor::Append>("a1");
    co_await actor.Mailbox(1).SendLocal<&MultiMailboxActor::Append>("b0");
    co_await actor.Mailbox(2).SendLocal<&MultiMailboxActor::Append>("c0");
    co_await actor.Mailbox(2).SendLocal<&MultiMailboxActor::Append>("c1");

    auto log = co_await actor.SendLocal<&MultiMailboxActor::GetLog>();
    EXPECT_EQ(log.size(), 5);
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, SingleMailboxQueueNumberOne) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(1, ex_actor::UnboundedThreadSafeMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    co_await actor.Mailbox(0).SendLocal<&MultiMailboxActor::Append>("only_queue");
    co_await actor.SendLocal<&MultiMailboxActor::Append>("default_queue");

    auto log = co_await actor.SendLocal<&MultiMailboxActor::GetLog>();
    EXPECT_EQ(log.size(), 2);
    EXPECT_EQ(log[0], "only_queue");
    EXPECT_EQ(log[1], "default_queue");
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

// ===========================
// Bounded Unsafe Queue Tests
// ===========================

TEST(MailboxTest, BoundedUnsafeQueueBasic) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(1, ex_actor::UnsafeOneSlotMailbox {})};
    auto actor = co_await ex_actor::Spawn<BoundedQueueActor>().WithConfig(config);

    co_await actor.SendLocal<&BoundedQueueActor::Increment>();
    co_await actor.SendLocal<&BoundedQueueActor::Increment>();
    co_await actor.SendLocal<&BoundedQueueActor::Increment>();

    int count = co_await actor.SendLocal<&BoundedQueueActor::GetCount>();
    EXPECT_EQ(count, 3);
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, BoundedUnsafeQueueWithSend) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(1, ex_actor::UnsafeOneSlotMailbox {})};
    auto actor = co_await ex_actor::Spawn<BoundedQueueActor>().WithConfig(config);

    int result = co_await actor.Send<&BoundedQueueActor::Add>(10);
    EXPECT_EQ(result, 10);

    result = co_await actor.Send<&BoundedQueueActor::Add>(20);
    EXPECT_EQ(result, 30);

    int count = co_await actor.Send<&BoundedQueueActor::GetCount>();
    EXPECT_EQ(count, 30);
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, BoundedUnsafeQueueMultipleQueues) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(3, ex_actor::UnsafeOneSlotMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    co_await actor.Mailbox(0).SendLocal<&MultiMailboxActor::Append>("bounded_mb0");
    co_await actor.Mailbox(1).SendLocal<&MultiMailboxActor::Append>("bounded_mb1");
    co_await actor.Mailbox(2).SendLocal<&MultiMailboxActor::Append>("bounded_mb2");

    auto log = co_await actor.SendLocal<&MultiMailboxActor::GetLog>();
    EXPECT_EQ(log.size(), 3);
    EXPECT_EQ(log[0], "bounded_mb0");
    EXPECT_EQ(log[1], "bounded_mb1");
    EXPECT_EQ(log[2], "bounded_mb2");
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, BoundedUnsafeQueueMultipleQueuesWithReturnValues) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(2, ex_actor::UnsafeOneSlotMailbox {})};
    auto actor = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(config);

    int result = co_await actor.Mailbox(0).Send<&MultiMailboxActor::Add>(10);
    EXPECT_EQ(result, 10);

    result = co_await actor.Mailbox(1).Send<&MultiMailboxActor::Add>(20);
    EXPECT_EQ(result, 30);

    result = co_await actor.Mailbox(0).Send<&MultiMailboxActor::Add>(5);
    EXPECT_EQ(result, 35);

    int sum = co_await actor.Send<&MultiMailboxActor::GetSum>();
    EXPECT_EQ(sum, 35);
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

// ===========================
// Nested Actor + Mailbox Tests
// ===========================

class MailboxParentActor {
 public:
  explicit MailboxParentActor(ex_actor::ActorRef<MultiMailboxActor> child) : child_(child) {}

  stdexec::task<void> DelegateToMailbox(size_t mailbox_index, const std::string& value) {
    co_await child_.Mailbox(mailbox_index).Send<&MultiMailboxActor::Append>(value);
  }

  stdexec::task<std::vector<std::string>> GetChildLog() {
    co_return co_await child_.Send<&MultiMailboxActor::GetLog>();
  }

 private:
  ex_actor::ActorRef<MultiMailboxActor> child_;
};

// ===========================
// ExActorShouldActivate Hook Tests
// ===========================

// Accumulates pushes in the hook; only permits activation once the configured threshold
// is reached. Exposes the hook observations via atomics captured by pointer so tests can
// inspect them even after the actor is destroyed.
class ThresholdActivationActor {
 public:
  struct Observations {
    std::atomic<size_t> hook_call_count {0};
    std::atomic<size_t> last_mailbox_index {SIZE_MAX};
    std::atomic<bool> destroy_signal_seen {false};
  };

  ThresholdActivationActor(Observations* observations, size_t activate_threshold)
      : observations_(observations), activate_threshold_(activate_threshold) {}

  bool ExActorShouldActivate(ex_actor::MailboxPushEvent event) {
    observations_->hook_call_count.fetch_add(1, std::memory_order_relaxed);
    if (event.is_destroy_signal) {
      observations_->destroy_signal_seen.store(true, std::memory_order_relaxed);
      return true;
    }
    observations_->last_mailbox_index.store(event.mailbox_index, std::memory_order_relaxed);
    size_t count_after = pending_pushes_.fetch_add(1, std::memory_order_relaxed) + 1;
    return count_after >= activate_threshold_;
  }

  void Increment() { processed_++; }
  int GetProcessed() const { return processed_; }

 private:
  Observations* observations_;
  size_t activate_threshold_;
  std::atomic<size_t> pending_pushes_ {0};
  int processed_ = 0;
};

// Gates activation per-mailbox: only pushes into `allowed_mailbox_` trigger activation.
// Pushes into other mailboxes accumulate and are drained (round-robin) when an allowed
// push finally activates the actor.
class PerMailboxGatingActor {
 public:
  explicit PerMailboxGatingActor(size_t allowed_mailbox) : allowed_mailbox_(allowed_mailbox) {}

  bool ExActorShouldActivate(ex_actor::MailboxPushEvent event) {
    if (event.is_destroy_signal) {
      return true;
    }
    return event.mailbox_index == allowed_mailbox_;
  }

  void Append(const std::string& value) { log_.push_back(value); }
  std::vector<std::string> GetLog() const { return log_; }

 private:
  size_t allowed_mailbox_;
  std::vector<std::string> log_;
};

TEST(MailboxTest, ShouldActivateHookGatesActivationUntilThreshold) {
  ThresholdActivationActor::Observations observations;
  auto coroutine = [&observations]() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<ThresholdActivationActor>(&observations, /*activate_threshold=*/size_t {3});

    // Fire three pushes without waiting; the first two should be vetoed by the hook and
    // the third should release the actor. Once activated, end-of-run re-activation drains
    // the remaining queued messages (one per round-robin pass through the single mailbox).
    stdexec::simple_counting_scope scope;
    stdexec::spawn(actor.SendLocal<&ThresholdActivationActor::Increment>() | ex_actor::DiscardResult(),
                   scope.get_token());
    stdexec::spawn(actor.SendLocal<&ThresholdActivationActor::Increment>() | ex_actor::DiscardResult(),
                   scope.get_token());
    stdexec::spawn(actor.SendLocal<&ThresholdActivationActor::Increment>() | ex_actor::DiscardResult(),
                   scope.get_token());
    co_await scope.join();

    int processed = co_await actor.SendLocal<&ThresholdActivationActor::GetProcessed>();
    EXPECT_EQ(processed, 3);

    // Hook must have been called once per push (3 Increments + 1 GetProcessed).
    EXPECT_EQ(observations.hook_call_count.load(), 4U);
    EXPECT_EQ(observations.last_mailbox_index.load(), 0U);
    EXPECT_FALSE(observations.destroy_signal_seen.load());
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();

  // Shutdown destroys the actor, which must flow through the hook with is_destroy_signal=true.
  EXPECT_TRUE(observations.destroy_signal_seen.load());
}

TEST(MailboxTest, ShouldActivateHookPerMailboxGating) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig config {.mailbox_configs =
                                      std::vector<ex_actor::MailboxConfig>(2, ex_actor::UnboundedThreadSafeMailbox {})};
    auto actor = co_await ex_actor::Spawn<PerMailboxGatingActor>(/*allowed_mailbox=*/1).WithConfig(std::move(config));

    stdexec::simple_counting_scope scope;
    // First push goes to mailbox 0; hook vetoes activation.
    stdexec::spawn(actor.Mailbox(0).SendLocal<&PerMailboxGatingActor::Append>("mb0_msg") | ex_actor::DiscardResult(),
                   scope.get_token());
    // Second push goes to mailbox 1; hook approves activation.
    // Round-robin draining should pick up the mb0 message as well.
    stdexec::spawn(actor.Mailbox(1).SendLocal<&PerMailboxGatingActor::Append>("mb1_msg") | ex_actor::DiscardResult(),
                   scope.get_token());
    co_await scope.join();

    auto log = co_await actor.Mailbox(1).SendLocal<&PerMailboxGatingActor::GetLog>();
    EXPECT_EQ(log.size(), 2U);
    if (log.size() >= 2) {
      EXPECT_EQ(log[0], "mb0_msg");
      EXPECT_EQ(log[1], "mb1_msg");
    }
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(MailboxTest, NestedActorDelegatesToChildMailboxes) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig child_config {
        .mailbox_configs = std::vector<ex_actor::MailboxConfig>(2, ex_actor::UnboundedThreadSafeMailbox {})};
    auto child = co_await ex_actor::Spawn<MultiMailboxActor>().WithConfig(child_config);
    auto parent = co_await ex_actor::Spawn<MailboxParentActor>(child);

    co_await parent.Send<&MailboxParentActor::DelegateToMailbox>(0, "delegated_mb0");
    co_await parent.Send<&MailboxParentActor::DelegateToMailbox>(1, "delegated_mb1");

    auto log = co_await parent.Send<&MailboxParentActor::GetChildLog>();
    EXPECT_EQ(log.size(), 2);
    EXPECT_EQ(log[0], "delegated_mb0");
    EXPECT_EQ(log[1], "delegated_mb1");
  };
  ex_actor::Init(/*thread_pool_size=*/4);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
