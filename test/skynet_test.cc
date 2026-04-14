#include <chrono>
#include <iostream>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

#include "ex_actor/api.h"

namespace ex = stdexec;
namespace logging = ex_actor::internal::log;

struct SkynetActor;

struct SkynetActor {
  stdexec::task<uint64_t> Process(int level, bool verbose);
};

stdexec::task<uint64_t> SkynetActor::Process(int level, bool verbose) {
  if (verbose) logging::Info("DEBUG: Process level {} start", level);
  if (level == 0) {
    if (verbose) logging::Info("DEBUG: Process level 0 returning");
    co_return 1ULL;
  }

  std::vector<ex_actor::ActorRef<SkynetActor>> children;
  children.reserve(10);

  stdexec::simple_counting_scope async_scope;

  using FutureType = decltype(stdexec::spawn_future(ex_actor::Spawn<SkynetActor>(), async_scope.get_token()));
  std::vector<FutureType> futures;
  futures.reserve(10);

  if (verbose) logging::Info("DEBUG: Creating children for level {}", level);
  for (int i = 0; i < 10; ++i) {
    futures.push_back(stdexec::spawn_future(ex_actor::Spawn<SkynetActor>(), async_scope.get_token()));
  }

  for (auto& future : futures) {
    children.push_back(co_await std::move(future));
  }

  auto make_child_sender = [&children, verbose, level](int index) {
    // test ephemeral stacks
    return children.at(index).Send<&SkynetActor::Process>(level - 1, false) | ex::then([verbose, index](uint64_t res) {
             if (verbose) logging::Info("Skynet Progress: {} subtree finished.", index + 1);
             return res;
           });
  };

  using ResultFutureType = decltype(stdexec::spawn_future(make_child_sender(0), async_scope.get_token()));
  std::vector<ResultFutureType> result_futures;
  result_futures.reserve(10);

  for (int i = 0; i < 10; ++i) {
    result_futures.push_back(stdexec::spawn_future(make_child_sender(i), async_scope.get_token()));
  }

  if (verbose) std::cout << "DEBUG: Awaiting children for level " << level << std::endl;
  uint64_t sum = 0;
  for (auto& future : result_futures) {
    sum += co_await std::move(future);
  }

  for (auto& child : children) {
    co_await ex_actor::DestroyActor(child);
  }

  if (verbose) std::cout << "DEBUG: Finished level " << level << ", sum=" << sum << std::endl;

  co_await async_scope.join();

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