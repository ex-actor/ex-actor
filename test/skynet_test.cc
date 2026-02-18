#include <chrono>
#include <iostream>
#include <tuple>
#include <vector>

#include <exec/task.hpp>
#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

#include "ex_actor/api.h"

namespace ex = stdexec;
namespace logging = ex_actor::internal::log;

struct SkynetActor;

struct SkynetActor {
  exec::task<uint64_t> Process(int level, bool verbose);
};

exec::task<uint64_t> SkynetActor::Process(int level, bool verbose) {
  if (verbose) logging::Info("DEBUG: Process level {} start", level);
  if (level == 0) {
    if (verbose) logging::Info("DEBUG: Process level 0 returning");
    co_return 1;
  }

  std::vector<ex_actor::ActorRef<SkynetActor>> children;
  children.reserve(10);

  exec::async_scope async_scope;

  using FutureType = decltype(async_scope.spawn_future(ex_actor::Spawn<SkynetActor>()));
  std::vector<FutureType> futures;
  futures.reserve(10);

  if (verbose) logging::Info("DEBUG: Creating children for level {}", level);
  for (int i = 0; i < 10; ++i) {
    futures.push_back(async_scope.spawn_future(ex_actor::Spawn<SkynetActor>()));
  }
  co_await async_scope.on_empty();
  for (auto& future : futures) {
    children.push_back(co_await std::move(future));
  }

  // Use async_scope to spawn all child tasks in parallel and collect results
  // This avoids the when_all limitation with many senders in newer stdexec versions
  exec::async_scope child_scope;

  auto make_child_sender = [&children, verbose, level](int index) {
    // test ephemeral stacks
    return children.at(index).Send<&SkynetActor::Process>(level - 1, false) | ex::then([verbose, index](uint64_t res) {
             if (verbose) logging::Info("Skynet Progress: {} subtree finished.", index + 1);
             return res;
           });
  };

  using ResultFutureType = decltype(child_scope.spawn_future(make_child_sender(0)));
  std::vector<ResultFutureType> result_futures;
  result_futures.reserve(10);

  for (int i = 0; i < 10; ++i) {
    result_futures.push_back(child_scope.spawn_future(make_child_sender(i)));
  }

  co_await child_scope.on_empty();

  if (verbose) std::cout << "DEBUG: Awaiting children for level " << level << std::endl;
  uint64_t sum = 0;
  for (auto& future : result_futures) {
    sum += co_await std::move(future);
  }

  for (auto& child : children) {
    co_await ex_actor::DestroyActor(child);
  }

  if (verbose) std::cout << "DEBUG: Finished level " << level << ", sum=" << sum << std::endl;
  co_return sum;
}

TEST(SkynetTest, ActorRegistryCanBeInvokeInsideActorMethods) {
  ex_actor::Init(/*thread_pool_size=*/2);
  auto [root] = stdexec::sync_wait(ex_actor::Spawn<SkynetActor>()).value();

  int depth = 4;

  logging::Info("Starting Skynet (Depth {}, Width 10)...", depth);
  auto start = std::chrono::high_resolution_clock::now();

  auto task = root.SendLocal<&SkynetActor::Process>(depth, true);
  auto [result] = ex::sync_wait(std::move(task)).value();
  ASSERT_EQ(result, std::pow(10, depth));

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  logging::Info("Result: {} ms", result);
  logging::Info("Time: {} ms", duration);
  ex_actor::Shutdown();
}