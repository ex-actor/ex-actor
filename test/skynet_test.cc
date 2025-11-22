#include <chrono>
#include <iostream>
#include <tuple>
#include <vector>

#include <exec/task.hpp>
#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

#include "ex_actor/api.h"

namespace ex = stdexec;

struct SkynetActor;

using RegistryType = ex_actor::ActorRegistry;

static RegistryType* g_registry = nullptr;

struct SkynetActor {
  exec::task<uint64_t> Process(int level, bool verbose);
};

exec::task<uint64_t> SkynetActor::Process(int level, bool verbose) {
  if (verbose) spdlog::info("DEBUG: Process level {} start", level);
  if (level == 0) {
    if (verbose) spdlog::info("DEBUG: Process level 0 returning");
    co_return 1;
  }

  std::vector<ex_actor::ActorRef<SkynetActor>> children;
  children.reserve(10);

  exec::async_scope async_scope;

  using FutureType = decltype(async_scope.spawn_future(g_registry->CreateActor<SkynetActor>()));
  std::vector<FutureType> futures;
  futures.reserve(10);

  if (verbose) spdlog::info("DEBUG: Creating children for level {}", level);
  for (int i = 0; i < 10; ++i) {
    futures.push_back(async_scope.spawn_future(g_registry->CreateActor<SkynetActor>()));
  }
  co_await async_scope.on_empty();
  for (auto& future : futures) {
    children.push_back(co_await std::move(future));
  }

  auto make_child_sender = [&children, verbose, level](int index) {
    // test ephemeral stacks
    return children.at(index).Send<&SkynetActor::Process>(level - 1, false) | ex::then([verbose, index](uint64_t res) {
             if (verbose) spdlog::info("Skynet Progress: {} subtree finished.", index + 1);
             return res;
           });
  };

  auto all = ex::when_all(make_child_sender(0), make_child_sender(1), make_child_sender(2), make_child_sender(3),
                          make_child_sender(4), make_child_sender(5), make_child_sender(6), make_child_sender(7),
                          make_child_sender(8), make_child_sender(9));

  auto sum_sender = std::move(all) | ex::then([](uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3, uint64_t r4,
                                                 uint64_t r5, uint64_t r6, uint64_t r7, uint64_t r8, uint64_t r9) {
                      return r0 + r1 + r2 + r3 + r4 + r5 + r6 + r7 + r8 + r9;
                    });

  if (verbose) std::cout << "DEBUG: Awaiting children for level " << level << std::endl;
  uint64_t sum = co_await std::move(sum_sender);

  for (auto& child : children) {
    co_await g_registry->DestroyActor(child);
  }

  if (verbose) std::cout << "DEBUG: Finished level " << level << ", sum=" << sum << std::endl;
  co_return sum;
}

TEST(SkynetTest, ActorRegistryCanBeInvokeInsideActorMethods) {
  int threads = 2;
  ex_actor::WorkSharingThreadPool thread_pool(threads);

  RegistryType registry(thread_pool.GetScheduler());
  g_registry = &registry;

  auto [root] = stdexec::sync_wait(registry.CreateActor<SkynetActor>()).value();

  int depth = 4;

  spdlog::info("Starting Skynet (Depth {}, Width 10)...", depth);
  auto start = std::chrono::high_resolution_clock::now();

  auto task = root.SendLocal<&SkynetActor::Process>(depth, true);
  auto [result] = ex::sync_wait(std::move(task)).value();
  ASSERT_EQ(result, std::pow(10, depth));

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  spdlog::info("Result: {} ms", result);
  spdlog::info("Time: {} ms", duration);
}