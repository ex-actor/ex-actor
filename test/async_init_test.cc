#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = ex_actor::ex;

struct ConstructorThreadRecorder {
  explicit ConstructorThreadRecorder(std::thread::id* out) { *out = std::this_thread::get_id(); }
  void Noop() {}
};

TEST(AsyncInitTest, ConstructorRunsOnCorrectSchedulerWithEnv) {
  ex_actor::WorkSharingThreadPool thread_pool1(1);
  ex_actor::WorkSharingThreadPool thread_pool2(1);
  ex_actor::SchedulerUnion scheduler_union(thread_pool1.GetScheduler(), thread_pool2.GetScheduler());

  // Get the thread ID that runs on each pool.
  auto get_tid = ex::schedule(scheduler_union.GetScheduler()) | ex::then([] { return std::this_thread::get_id(); });
  auto [tid_pool1] = ex::sync_wait(get_tid | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 0})).value();
  auto [tid_pool2] = ex::sync_wait(get_tid | ex::write_env(ex::prop {ex_actor::get_scheduler_index, 1})).value();
  ASSERT_NE(tid_pool1, tid_pool2);

  ex_actor::Init(scheduler_union.GetScheduler());
  auto coroutine = [&]() -> stdexec::task<void> {
    std::thread::id ctor_thread;
    auto actor = co_await ex_actor::Spawn<ConstructorThreadRecorder>(&ctor_thread).WithConfig({.scheduler_index = 1});
    // Constructor should have run on thread pool 2, not thread pool 1.
    EXPECT_EQ(ctor_thread, tid_pool2);
    EXPECT_NE(ctor_thread, tid_pool1);
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

struct SlowActor {
  explicit SlowActor(std::atomic_int* concurrent_count, std::atomic_int* max_concurrent) {
    int cur = concurrent_count->fetch_add(1, std::memory_order_relaxed) + 1;
    int prev_max = max_concurrent->load(std::memory_order_relaxed);
    while (cur > prev_max && !max_concurrent->compare_exchange_weak(prev_max, cur)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    concurrent_count->fetch_sub(1, std::memory_order_relaxed);
  }
  void Noop() {}
};

struct ThrowingActor {
  explicit ThrowingActor() { throw std::runtime_error("ctor failed"); }
  void Noop() {}
};

struct TrivialActor {
  void Noop() {}
};

// Regression test for commit 4b95ee8: when a named actor's constructor throws
// during InitUserClassInstance, the registry must roll back the name->id
// mapping so the name can be reused.
TEST(AsyncInitTest, NameRegistrationRolledBackWhenConstructorThrows) {
  constexpr const char* kName = "rollback_name";
  ex_actor::Init(/*thread_pool_size=*/1);
  auto coroutine = [kName]() -> stdexec::task<void> {
    bool threw = false;
    try {
      // GCC < 13 double-frees a temporary ActorConfig (heap-allocated actor_name) used directly
      // in a co_await expression, so bind it to a named variable first. See actor_config.h.
      ex_actor::ActorConfig config {.actor_name = kName};
      co_await ex_actor::Spawn<ThrowingActor>().WithConfig(config);
    } catch (const std::exception&) {
      threw = true;
    }
    EXPECT_TRUE(threw);

    // The name mapping should have been erased on failure.
    auto looked_up = co_await ex_actor::GetActorRefByName<ThrowingActor>(kName);
    EXPECT_FALSE(looked_up.has_value());

    // The name is free again, so spawning a different actor with the same name
    // must succeed instead of failing the duplicate-name check.
    ex_actor::ActorConfig config {.actor_name = kName};
    auto actor = co_await ex_actor::Spawn<TrivialActor>().WithConfig(config);
    EXPECT_FALSE(actor.IsEmpty());
    auto looked_up_again = co_await ex_actor::GetActorRefByName<TrivialActor>(kName);
    EXPECT_TRUE(looked_up_again.has_value());
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

// An actor that remembers the unique seed it was constructed with, so we can
// detect whether the ActorRef handed back to the caller still points at the
// object that was created for it (and not one clobbered by an id collision).
struct SeededActor {
  explicit SeededActor(int seed) : seed(seed) {}
  int GetSeed() const { return seed; }
  int seed = 0;
};

// Regression test: concurrent spawns must not be handed the same actor id.
//
// The bug:
//   1. SpawnLocal draws a random id, then `co_await`s the constructor, then
//      inserts the actor into actor_id_to_actor_.
//   2. That `co_await` suspends, so the registry keeps draining its mailbox and
//      starts more spawns before the first one has inserted its id.
//   3. GenerateRandomActorId dedups only against actor_id_to_actor_, so it
//      can't see those in-flight ids and may hand out a duplicate.
//   4. The second insert overwrites and destroys the first Actor while an
//      ActorRef to it is already live -> use-after-free on the next Send.
//
// The fix inserts the id *before* the `co_await` (and rolls it back if the
// constructor throws), so in-flight ids are visible to GenerateRandomActorId.
//
// This test spawns many actors concurrently, each carrying a unique seed, then
// checks that all ids are distinct and every ref still reports its own seed.
// With the bug, ids collide and the seed read-back mismatches (or segfaults).
TEST(AsyncInitTest, ConcurrentSpawnsDoNotCollideActorIds) {
  ex_actor::Init(/*thread_pool_size=*/4);
  auto coroutine = []() -> stdexec::task<void> {
    constexpr int kNumActors = 100000;
    std::vector<ex_actor::ActorRef<SeededActor>> refs(kNumActors);

    stdexec::simple_counting_scope scope;
    for (int i = 0; i < kNumActors; ++i) {
      stdexec::spawn(ex_actor::Spawn<SeededActor>(i) | stdexec::then([&refs, i](auto ref) { refs[i] = ref; }) |
                         ex_actor::DiscardResult(),
                     scope.get_token());
    }
    co_await scope.join();

    // Every actor must have a distinct id; a collision means one actor's slot
    // in the registry was overwritten (and its Actor destroyed) by another.
    std::unordered_set<uint64_t> seen_ids;
    seen_ids.reserve(kNumActors);
    for (int i = 0; i < kNumActors; ++i) {
      EXPECT_FALSE(refs[i].IsEmpty()) << "spawn " << i << " produced an empty ref";
      bool inserted = seen_ids.insert(refs[i].GetActorId()).second;
      EXPECT_TRUE(inserted) << "duplicate actor id for spawn " << i;
    }

    // Each ref must still resolve to the actor constructed with its own seed.
    // A clobbered ref would read freed memory and return the wrong seed.
    for (int i = 0; i < kNumActors; ++i) {
      int seed = co_await refs[i].SendLocal<&SeededActor::GetSeed>();
      EXPECT_EQ(seed, i) << "ref " << i << " points at the wrong actor (id collision use-after-free)";
    }
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(AsyncInitTest, ConstructorsRunInParallel) {
  ex_actor::Init(/*thread_pool_size=*/4);
  auto coroutine = []() -> stdexec::task<void> {
    std::atomic_int concurrent_count = 0;
    std::atomic_int max_concurrent = 0;

    stdexec::simple_counting_scope scope;
    constexpr int kNumActors = 4;

    for (int i = 0; i < kNumActors; ++i) {
      stdexec::spawn(ex_actor::Spawn<SlowActor>(&concurrent_count, &max_concurrent) | ex_actor::DiscardResult(),
                     scope.get_token());
    }
    co_await scope.join();

    // If constructors ran in parallel, max_concurrent should be > 1.
    EXPECT_GT(max_concurrent.load(), 1);
  };
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
