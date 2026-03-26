// Regression test: when user actors occupy all threads in the scheduler,
// the MessageBroker must still be able to run heartbeats on its dedicated
// control-plane thread pool. Before the fix, the broker shared the same
// thread pool as user actors, so saturating it caused heartbeat timeouts.

#include <barrier>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/remote_handler_registry.h"

class BusyActor {
 public:
  static BusyActor Create() { return BusyActor(); }

  void BusyWork(uint64_t duration_ms) { std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms)); }
};
EXA_REMOTE(&BusyActor::Create, &BusyActor::BusyWork);

// Two-node cluster where every worker thread is blocked by user actors.
// The cluster must remain healthy (heartbeat must not timeout).
TEST(ControlPlaneIsolationTest, HeartbeatSurvivesWhenAllWorkerThreadsBusy) {
  constexpr size_t kWorkerThreads = 2;
  constexpr uint64_t kHeartbeatTimeoutMs = 2000;
  constexpr uint64_t kGossipIntervalMs = 200;
  constexpr uint64_t kBusyDurationMs = 3000;

  std::barrier bar {2};

  auto node_main = [&](size_t index) -> exec::task<void> {
    std::vector<std::string> addresses = {"tcp://127.0.0.1:5401", "tcp://127.0.0.1:5402"};
    ex_actor::ClusterConfig cluster_config {
        .listen_address = addresses.at(index),
        .contact_node_address = (index == 0) ? "" : addresses.at(0),
        .network_config =
            {
                .heartbeat_timeout_ms = kHeartbeatTimeoutMs,
                .gossip_interval_ms = kGossipIntervalMs,
            },
    };
    ex_actor::ActorRegistry registry(/*thread_pool_size=*/kWorkerThreads);
    co_await registry.StartOrJoinCluster(cluster_config);

    auto [cluster_state, condition_met] =
        co_await registry.WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                           /*timeout_ms=*/5000);
    EXPECT_TRUE(condition_met);
    EXPECT_GE(cluster_state.nodes.size(), 2U);
    if (cluster_state.nodes.size() < 2U) {
      bar.arrive_and_wait();
      co_return;
    }

    // Spawn enough busy actors to saturate all worker threads.
    // Each actor will sleep for kBusyDurationMs, which is longer than the heartbeat timeout.
    // If the broker shared the worker pool, it couldn't send/receive heartbeats and the
    // other node would be considered dead.
    exec::async_scope scope;
    for (size_t i = 0; i < kWorkerThreads * 2; ++i) {
      auto actor = co_await registry.Spawn<BusyActor>();
      scope.spawn(actor.Send<&BusyActor::BusyWork>(kBusyDurationMs));
    }

    // Wait long enough that the heartbeat would have timed out if the broker
    // were starved (kBusyDurationMs > kHeartbeatTimeoutMs).
    std::this_thread::sleep_for(std::chrono::milliseconds(kBusyDurationMs));

    // The cluster should still be healthy: both nodes alive.
    auto [state_after, still_connected] =
        co_await registry.WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                           /*timeout_ms=*/3000);
    EXPECT_TRUE(still_connected) << "Cluster lost connectivity — heartbeat was likely starved by busy actors";
    EXPECT_GE(state_after.nodes.size(), 2U);

    co_await scope.on_empty();
    bar.arrive_and_wait();
  };

  std::jthread node_0([&] { stdexec::sync_wait(node_main(0)); });
  std::jthread node_1([&] { stdexec::sync_wait(node_main(1)); });

  node_0.join();
  node_1.join();
}
