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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kUnboundedThreadSafeQueue, .mailbox_number = 3}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kUnboundedThreadSafeQueue, .mailbox_number = 2}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kUnboundedThreadSafeQueue, .mailbox_number = 2}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kUnboundedThreadSafeQueue, .mailbox_number = 3}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kUnboundedThreadSafeQueue, .mailbox_number = 1}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kBoundedUnsafeQueue, .mailbox_number = 1, .mailbox_size = 128}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kBoundedUnsafeQueue, .mailbox_number = 1, .mailbox_size = 128}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kBoundedUnsafeQueue, .mailbox_number = 3, .mailbox_size = 64}};
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
    ex_actor::ActorConfig config {
        .mailbox_config = {.type = ex_actor::MailboxType::kBoundedUnsafeQueue, .mailbox_number = 2, .mailbox_size = 64}};
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

TEST(MailboxTest, NestedActorDelegatesToChildMailboxes) {
  auto coroutine = []() -> stdexec::task<void> {
    ex_actor::ActorConfig child_config {
        .mailbox_config = {.type = ex_actor::MailboxType::kUnboundedThreadSafeQueue, .mailbox_number = 2}};
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

// ===========================
// FlatBoundedUnsafeQueues Unit Tests
// ===========================

TEST(FlatBoundedUnsafeQueuesTest, BasicPushPop) {
  ex_actor::internal::FlatBoundedUnsafeQueues<int> queues(/*queue_count=*/2, /*queue_size=*/4);

  EXPECT_TRUE(queues.Push(0, 10));
  EXPECT_TRUE(queues.Push(0, 20));
  EXPECT_TRUE(queues.Push(1, 30));

  auto val = queues.TryPop(0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), 10);

  val = queues.TryPop(0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), 20);

  val = queues.TryPop(1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), 30);

  val = queues.TryPop(0);
  EXPECT_FALSE(val.has_value());

  val = queues.TryPop(1);
  EXPECT_FALSE(val.has_value());
}

TEST(FlatBoundedUnsafeQueuesTest, FullQueueRejectsPush) {
  ex_actor::internal::FlatBoundedUnsafeQueues<int> queues(/*queue_count=*/1, /*queue_size=*/2);

  EXPECT_TRUE(queues.Push(0, 1));
  EXPECT_TRUE(queues.Push(0, 2));
  EXPECT_FALSE(queues.Push(0, 3));

  EXPECT_EQ(queues.Size(0), 2);
  EXPECT_TRUE(queues.Full(0));
}

TEST(FlatBoundedUnsafeQueuesTest, QueuesAreIndependent) {
  ex_actor::internal::FlatBoundedUnsafeQueues<int> queues(/*queue_count=*/3, /*queue_size=*/2);

  EXPECT_TRUE(queues.Push(0, 100));
  EXPECT_TRUE(queues.Push(0, 200));
  EXPECT_FALSE(queues.Push(0, 300));

  EXPECT_TRUE(queues.Push(1, 10));
  EXPECT_FALSE(queues.Full(1));
  EXPECT_TRUE(queues.Push(1, 20));
  EXPECT_TRUE(queues.Full(1));

  EXPECT_TRUE(queues.Empty(2));
  EXPECT_TRUE(queues.Push(2, 1));
  EXPECT_FALSE(queues.Empty(2));

  auto val = queues.TryPop(0);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value(), 100);
  EXPECT_EQ(queues.Size(0), 1);
}

TEST(FlatBoundedUnsafeQueuesTest, WrapAround) {
  ex_actor::internal::FlatBoundedUnsafeQueues<int> queues(/*queue_count=*/1, /*queue_size=*/3);

  EXPECT_TRUE(queues.Push(0, 1));
  EXPECT_TRUE(queues.Push(0, 2));
  EXPECT_TRUE(queues.Push(0, 3));

  auto val = queues.TryPop(0);
  EXPECT_EQ(val.value(), 1);

  val = queues.TryPop(0);
  EXPECT_EQ(val.value(), 2);

  EXPECT_TRUE(queues.Push(0, 4));
  EXPECT_TRUE(queues.Push(0, 5));
  EXPECT_FALSE(queues.Push(0, 6));

  val = queues.TryPop(0);
  EXPECT_EQ(val.value(), 3);
  val = queues.TryPop(0);
  EXPECT_EQ(val.value(), 4);
  val = queues.TryPop(0);
  EXPECT_EQ(val.value(), 5);
  EXPECT_FALSE(queues.TryPop(0).has_value());
}

TEST(FlatBoundedUnsafeQueuesTest, QueueCountAndSize) {
  ex_actor::internal::FlatBoundedUnsafeQueues<int> queues(/*queue_count=*/5, /*queue_size=*/10);
  EXPECT_EQ(queues.QueueCount(), 5);
  EXPECT_EQ(queues.QueueSize(), 10);
}
