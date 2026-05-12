#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = ex_actor::ex;

using testing::HasSubstr;
using testing::Property;
using testing::Throws;

namespace {
uint64_t ThisThreadIdHash() { return std::hash<std::thread::id> {}(std::this_thread::get_id()); }
}  // namespace

class ProbeActor {
 public:
  ProbeActor(std::atomic_uint64_t* ctor_thread_id, std::atomic_uint64_t* hook_thread_id)
      : hook_thread_id_(hook_thread_id) {
    ctor_thread_id->store(ThisThreadIdHash(), std::memory_order_release);
  }

  static ProbeActor Create(std::atomic_uint64_t* ctor_thread_id, std::atomic_uint64_t* hook_thread_id) {
    return ProbeActor(ctor_thread_id, hook_thread_id);
  }

  void ExActorOnSpawned(const ex_actor::ActorRuntimeInfo<ProbeActor>& /*info*/) {
    hook_thread_id_->store(ThisThreadIdHash(), std::memory_order_release);
  }

  uint64_t GetThreadId() const { return ThisThreadIdHash(); }

 private:
  std::atomic_uint64_t* hook_thread_id_ = nullptr;
};

TEST(SpawnConstructionEnvTest, CtorRunsOnConfiguredScheduler) {
  ex_actor::WorkSharingThreadPool pool0(1);
  ex_actor::WorkSharingThreadPool pool1(1);
  ex_actor::SchedulerUnion scheduler_union(pool0.GetScheduler(), pool1.GetScheduler());

  std::atomic_uint64_t ctor_id = 0;
  std::atomic_uint64_t hook_id = 0;

  auto coroutine = [&]() -> stdexec::task<void> {
    auto actor =
        co_await ex_actor::Spawn<ProbeActor>(&ctor_id, &hook_id).WithConfig({.scheduler_index = 1});
    auto method_id = co_await actor.Send<&ProbeActor::GetThreadId>();
    EXPECT_EQ(ctor_id.load(), method_id);
  };

  ex_actor::Init(scheduler_union.GetScheduler());
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SpawnConstructionEnvTest, CtorViaCreateFnRunsOnConfiguredScheduler) {
  ex_actor::WorkSharingThreadPool pool0(1);
  ex_actor::WorkSharingThreadPool pool1(1);
  ex_actor::SchedulerUnion scheduler_union(pool0.GetScheduler(), pool1.GetScheduler());

  std::atomic_uint64_t ctor_id = 0;
  std::atomic_uint64_t hook_id = 0;

  auto coroutine = [&]() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<&ProbeActor::Create>(&ctor_id, &hook_id)
                     .WithConfig({.scheduler_index = 1});
    auto method_id = co_await actor.Send<&ProbeActor::GetThreadId>();
    EXPECT_EQ(ctor_id.load(), method_id);
  };

  ex_actor::Init(scheduler_union.GetScheduler());
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(SpawnConstructionEnvTest, OnSpawnedRunsOnConfiguredScheduler) {
  ex_actor::WorkSharingThreadPool pool0(1);
  ex_actor::WorkSharingThreadPool pool1(1);
  ex_actor::SchedulerUnion scheduler_union(pool0.GetScheduler(), pool1.GetScheduler());

  std::atomic_uint64_t ctor_id = 0;
  std::atomic_uint64_t hook_id = 0;

  auto coroutine = [&]() -> stdexec::task<void> {
    auto actor =
        co_await ex_actor::Spawn<ProbeActor>(&ctor_id, &hook_id).WithConfig({.scheduler_index = 1});
    auto method_id = co_await actor.Send<&ProbeActor::GetThreadId>();
    EXPECT_EQ(hook_id.load(), method_id);
  };

  ex_actor::Init(scheduler_union.GetScheduler());
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

class ThrowingCtorActor {
 public:
  ThrowingCtorActor() { throw std::runtime_error("ctor failed"); }
};

TEST(SpawnConstructionEnvTest, CtorThrowPropagatesAndRegistryStaysClean) {
  auto coroutine = [&]() -> stdexec::task<void> {
    EXPECT_THAT(
        [] { ex::sync_wait(ex_actor::Spawn<ThrowingCtorActor>().WithConfig({.actor_name = "dup"})); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("ctor failed"))));

    auto found = co_await ex_actor::GetActorRefByName<ThrowingCtorActor>("dup");
    EXPECT_FALSE(found.has_value());

    // Re-spawning under the same name must succeed: the failed reservation should have been
    // rolled back. The second call still throws "ctor failed", confirming the name was not
    // already-taken.
    EXPECT_THAT(
        [] { ex::sync_wait(ex_actor::Spawn<ThrowingCtorActor>().WithConfig({.actor_name = "dup"})); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("ctor failed"))));
    co_return;
  };

  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

class MoveOnlyArgActor {
 public:
  explicit MoveOnlyArgActor(std::unique_ptr<int> value) : value_(std::move(value)) {}

  int GetValue() const { return *value_; }

 private:
  std::unique_ptr<int> value_;
};

TEST(SpawnConstructionEnvTest, MoveOnlyCtorArg) {
  auto coroutine = []() -> stdexec::task<void> {
    auto actor = co_await ex_actor::Spawn<MoveOnlyArgActor>(std::make_unique<int>(42));
    auto value = co_await actor.Send<&MoveOnlyArgActor::GetValue>();
    EXPECT_EQ(value, 42);
  };

  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

class ThrowingHookActor {
 public:
  void ExActorOnSpawned(const ex_actor::ActorRuntimeInfo<ThrowingHookActor>& /*info*/) {
    throw std::runtime_error("hook failed");
  }
};

TEST(SpawnConstructionEnvTest, HookThrowPropagatesAndRegistryStaysClean) {
  auto coroutine = []() -> stdexec::task<void> {
    EXPECT_THAT([] { ex::sync_wait(ex_actor::Spawn<ThrowingHookActor>().WithConfig({.actor_name = "dup"})); },
                Throws<std::exception>(Property(&std::exception::what, HasSubstr("hook failed"))));

    auto found = co_await ex_actor::GetActorRefByName<ThrowingHookActor>("dup");
    EXPECT_FALSE(found.has_value());
    co_return;
  };

  ex_actor::Init(/*thread_pool_size=*/2);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

class TrivialActor {
 public:
  int GetValue() const { return 0; }
};

TEST(SpawnConstructionEnvTest, ConcurrentNamedSpawnDuplicate) {
  ex_actor::Init(/*thread_pool_size=*/4);

  std::atomic_int success_count = 0;
  std::atomic_int failure_count = 0;

  auto worker = [&] {
    try {
      ex::sync_wait(ex_actor::Spawn<TrivialActor>().WithConfig({.actor_name = "race"}));
      success_count.fetch_add(1, std::memory_order_acq_rel);
    } catch (const std::exception&) {
      failure_count.fetch_add(1, std::memory_order_acq_rel);
    }
  };

  std::thread t1(worker);
  std::thread t2(worker);
  t1.join();
  t2.join();

  EXPECT_EQ(success_count.load(), 1);
  EXPECT_EQ(failure_count.load(), 1);

  ex_actor::Shutdown();
}
