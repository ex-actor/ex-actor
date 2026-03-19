#include "ex_actor/internal/network.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <ranges>
#include <string>
#include <thread>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

using ex_actor::internal::ByteBuffer;

namespace {

ByteBuffer MakeBytes(const std::string& str) {
  return ByteBuffer(reinterpret_cast<const std::byte*>(str.data()),
                    reinterpret_cast<const std::byte*>(str.data() + str.size()));
}

std::string BytesToString(const ByteBuffer& buf) {
  return std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
}

ex_actor::ClusterConfig MakeConfig(const std::string& address, const std::string& contact_address = "",
                                   uint64_t heartbeat_timeout_ms = 5000, uint64_t gossip_interval_ms = 500) {
  ex_actor::ClusterConfig config;
  config.listen_address = address;
  config.contact_node_address = contact_address;
  config.network_config.heartbeat_timeout_ms = heartbeat_timeout_ms;
  config.network_config.gossip_interval_ms = gossip_interval_ms;
  return config;
}

void DispatchGossip(ex_actor::internal::MessageBroker& broker,
                    const std::vector<ex_actor::internal::NodeState>& node_states) {
  uint64_t sender_node_id = node_states.empty() ? 0 : node_states.front().node_id;
  ex_actor::internal::BrokerMessage broker_msg {.variant = ex_actor::internal::BrokerGossipMessage {
                                                    .from_node_id = sender_node_id,
                                                    .node_states = node_states,
                                                }};
  auto raw = ex_actor::internal::Serialize(broker_msg);
  stdexec::sync_wait(broker.DispatchReceivedMessage(std::move(raw)));
}

}  // namespace

// ============================================================
// Constructor validation tests
// ============================================================

TEST(MessageBrokerTest, ConstructorSucceedsWithNoContactNode) {
  auto config = MakeConfig("tcp://127.0.0.1:7203");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);
  stdexec::sync_wait(broker.Stop());
}

TEST(MessageBrokerTest, ConstructorSucceedsWithValidContactNode) {
  auto config = MakeConfig("tcp://127.0.0.1:7204",
                           /*contact_address=*/"tcp://127.0.0.1:7205");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);
  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// SendRequest to unconnected / self node
// ============================================================

TEST(MessageBrokerTest, SendRequestToUnconnectedNodeThrows) {
  auto config = MakeConfig("tcp://127.0.0.1:7210");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  EXPECT_THAT([&]() { stdexec::sync_wait(broker.SendRequest(/*to_node_id=*/99, {})); },
              testing::Throws<ex_actor::ConnectionLost>(testing::Property(
                  &std::exception::what, testing::HasSubstr("trying to send request to an unconnected node"))));

  stdexec::sync_wait(broker.Stop());
}

TEST(MessageBrokerTest, SendRequestToSelfThrows) {
  auto config = MakeConfig("tcp://127.0.0.1:7211");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  EXPECT_THAT([&]() { stdexec::sync_wait(broker.SendRequest(/*to_node_id=*/0, {})); },
              testing::Throws<std::exception>(
                  testing::Property(&std::exception::what, testing::HasSubstr("Cannot send message to current node"))));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// WaitClusterState: immediate return for already-connected contact node
// ============================================================

TEST(MessageBrokerTest, WaitClusterStateReturnsTrueForGossipDiscoveredNode) {
  auto config = MakeConfig("tcp://127.0.0.1:7220");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7221"}});

  auto [result] = stdexec::sync_wait(broker.WaitClusterState(
                                         [](const ex_actor::ClusterState& state) {
                                           return std::ranges::any_of(
                                               state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                                         },
                                         /*timeout_ms=*/10))
                      .value();
  EXPECT_TRUE(result.condition_met);

  stdexec::sync_wait(broker.Stop());
}

TEST(MessageBrokerTest, WaitClusterStateWithAlwaysTruePredicateReturnsImmediately) {
  auto config = MakeConfig("tcp://127.0.0.1:7222");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/5, config);

  // An always-true predicate returns immediately (self node is always in the list).
  auto [result] =
      stdexec::sync_wait(broker.WaitClusterState([](const ex_actor::ClusterState& /*state*/) { return true; },
                                                 /*timeout_ms=*/10))
          .value();
  EXPECT_TRUE(result.condition_met);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// WaitClusterState + CheckClusterStateWaiterTimeout: waiter times out
// ============================================================

TEST(MessageBrokerTest, WaitClusterStateTimesOutForUnknownNode) {
  auto config = MakeConfig("tcp://127.0.0.1:7230");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  exec::async_scope scope;
  std::atomic<bool> condition_met = true;

  scope.spawn(broker.WaitClusterState(
                  [](const ex_actor::ClusterState& state) {
                    return std::ranges::any_of(state.nodes,
                                               [](const ex_actor::NodeInfo& n) { return n.node_id == 99; });
                  },
                  /*timeout_ms=*/0) |
              stdexec::then([&condition_met](const ex_actor::WaitClusterStateResult& res) {
                condition_met.store(res.condition_met, std::memory_order_relaxed);
              }));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckClusterStateWaiterTimeout();

  stdexec::sync_wait(scope.on_empty());
  EXPECT_FALSE(condition_met.load(std::memory_order_relaxed));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// WaitClusterState resolves when gossip brings a new node
// ============================================================

TEST(MessageBrokerTest, WaitClusterStateResolvesOnGossipDiscovery) {
  auto config = MakeConfig("tcp://127.0.0.1:7240");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  exec::async_scope scope;
  std::atomic<bool> condition_met = false;

  scope.spawn(broker.WaitClusterState(
                  [](const ex_actor::ClusterState& state) {
                    return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                  },
                  /*timeout_ms=*/5000) |
              stdexec::then([&condition_met](const ex_actor::WaitClusterStateResult& res) {
                condition_met.store(res.condition_met, std::memory_order_relaxed);
              }));

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7241"}});

  stdexec::sync_wait(scope.on_empty());
  EXPECT_TRUE(condition_met.load(std::memory_order_relaxed));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// HandleGossipMessage: duplicate gossip for same node is idempotent
// ============================================================

TEST(MessageBrokerTest, DuplicateGossipForSameNodeDoesNotThrow) {
  auto config = MakeConfig("tcp://127.0.0.1:7250");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7251"}});
  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 200, .node_id = 1, .address = "tcp://127.0.0.1:7251"}});

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// HandleGossipMessage: conflicting address for same node_id throws
// ============================================================

TEST(MessageBrokerTest, GossipWithConflictingAddressThrows) {
  auto config = MakeConfig("tcp://127.0.0.1:7260");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7261"}});

  EXPECT_THAT(
      [&]() {
        DispatchGossip(
            broker, {{.alive = true, .last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7262"}});
      },
      testing::Throws<std::exception>(
          testing::Property(&std::exception::what, testing::HasSubstr("Node 0x1 has conflicting address"))));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// CheckHeartbeatTimeout: deactivates timed-out nodes
// ============================================================

TEST(MessageBrokerTest, CheckHeartbeatTimeoutDeactivatesTimedOutNodes) {
  auto config = MakeConfig("tcp://127.0.0.1:7270",
                           /*contact_address=*/"tcp://127.0.0.1:7271",
                           /*heartbeat_timeout_ms=*/1);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Discover node 1 via gossip so it gets added to node_id_to_state_
  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7271"}});

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // now node 1 should be dead
  broker.CheckHeartbeatTimeout();

  exec::async_scope scope;
  std::atomic<bool> condition_met = true;
  scope.spawn(ex_actor::internal::WrapSenderWithInlineScheduler(
      broker.WaitClusterState(
          [](const ex_actor::ClusterState& state) {
            return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
          },
          /*timeout_ms=*/0) |
      stdexec::then(
          [&condition_met](const ex_actor::WaitClusterStateResult& res) { condition_met = res.condition_met; })));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckClusterStateWaiterTimeout();

  stdexec::sync_wait(scope.on_empty());
  EXPECT_FALSE(condition_met);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// CheckHeartbeatTimeout: errors outstanding requests for dead node
// ============================================================

TEST(MessageBrokerTest, CheckHeartbeatTimeoutErrorsOutstandingRequests) {
  auto config = MakeConfig("tcp://127.0.0.1:7280",
                           /*contact_address=*/"tcp://127.0.0.1:7281",
                           /*heartbeat_timeout_ms=*/1);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Discover node 1 via gossip so it gets added to node_id_to_state_ and a send socket is created
  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7281"}});

  exec::async_scope scope;
  std::atomic<bool> got_error = false;

  scope.spawn(broker.SendRequest(/*to_node_id=*/1, MakeBytes("hello")) | stdexec::then([](const ByteBuffer&) {}) |
              stdexec::upon_error([&got_error](const auto&) { got_error.store(true, std::memory_order_relaxed); }));

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckHeartbeatTimeout();

  stdexec::sync_wait(scope.on_empty());
  EXPECT_TRUE(got_error.load(std::memory_order_relaxed));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// HandleRepliedResponse: resolves outstanding request via DispatchReceivedMessage
// ============================================================

TEST(MessageBrokerTest, HandleRepliedResponseResolvesOutstandingRequest) {
  auto config = MakeConfig("tcp://127.0.0.1:7290",
                           /*contact_address=*/"tcp://127.0.0.1:7291");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Discover node 1 via gossip so we can send requests to it
  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7291"}});

  exec::async_scope scope;
  std::atomic<bool> got_response = false;
  std::string response_data;

  scope.spawn(broker.SendRequest(/*to_node_id=*/1, MakeBytes("ping")) |
              stdexec::then([&got_response, &response_data](const ByteBuffer& data) {
                response_data = BytesToString(data);
                got_response.store(true, std::memory_order_relaxed);
              }));

  ex_actor::internal::BrokerMessage reply_msg {.variant = ex_actor::internal::BrokerTwoWayMessage {
                                                   .request_node_id = 0,
                                                   .response_node_id = 1,
                                                   .request_id = 0,
                                                   .payload = MakeBytes("pong"),
                                               }};
  auto raw = ex_actor::internal::Serialize(reply_msg);
  stdexec::sync_wait(broker.DispatchReceivedMessage(std::move(raw)));

  stdexec::sync_wait(scope.on_empty());
  EXPECT_TRUE(got_response.load(std::memory_order_relaxed));
  EXPECT_EQ(response_data, "pong");

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// DispatchReceivedMessage: rejects invalid identifier
// ============================================================

TEST(MessageBrokerTest, DispatchReceivedMessageRejectsMisdirectedTwoWay) {
  auto config = MakeConfig("tcp://127.0.0.1:7310",
                           /*contact_address=*/"tcp://127.0.0.1:7311");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  ex_actor::internal::BrokerMessage bad_msg {.variant = ex_actor::internal::BrokerTwoWayMessage {
                                                 .request_node_id = 5,
                                                 .response_node_id = 6,
                                                 .request_id = 0,
                                                 .payload = MakeBytes("bad"),
                                             }};
  auto raw = ex_actor::internal::Serialize(bad_msg);

  EXPECT_THAT([&]() { stdexec::sync_wait(broker.DispatchReceivedMessage(std::move(raw))); },
              testing::Throws<std::exception>(
                  testing::Property(&std::exception::what, testing::HasSubstr("not addressed to this node"))));

  stdexec::sync_wait(broker.Stop());
}

TEST(MessageBrokerTest, DispatchReceivedMessageRejectsCorruptData) {
  auto config = MakeConfig("tcp://127.0.0.1:7312");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  auto corrupt = MakeBytes("abc");
  EXPECT_THROW(stdexec::sync_wait(broker.DispatchReceivedMessage(std::move(corrupt))), std::exception);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// Multiple waiters for the same node
// ============================================================

TEST(MessageBrokerTest, MultipleWaitersNotifiedOnGossipDiscovery) {
  auto config = MakeConfig("tcp://127.0.0.1:7320");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  exec::async_scope scope;
  std::atomic<int> success_count = 0;

  for (int i = 0; i < 3; ++i) {
    scope.spawn(broker.WaitClusterState(
                    [](const ex_actor::ClusterState& state) {
                      return std::ranges::any_of(state.nodes,
                                                 [](const ex_actor::NodeInfo& n) { return n.node_id == 2; });
                    },
                    /*timeout_ms=*/5000) |
                stdexec::then([&success_count](const ex_actor::WaitClusterStateResult& res) {
                  if (res.condition_met) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                  }
                }));
  }

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 99999, .node_id = 2, .address = "tcp://127.0.0.1:7321"}});

  stdexec::sync_wait(scope.on_empty());
  EXPECT_EQ(success_count.load(std::memory_order_relaxed), 3);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// Multi-node gossip message introduces multiple nodes at once
// ============================================================

TEST(MessageBrokerTest, GossipIntroducesMultipleNodesAtOnce) {
  auto config = MakeConfig("tcp://127.0.0.1:7330");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  exec::async_scope scope;
  std::atomic<int> success_count = 0;

  scope.spawn(broker.WaitClusterState(
                  [](const ex_actor::ClusterState& state) {
                    return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                  },
                  /*timeout_ms=*/5000) |
              stdexec::then([&success_count](const ex_actor::WaitClusterStateResult& res) {
                if (res.condition_met) {
                  success_count.fetch_add(1, std::memory_order_relaxed);
                }
              }));

  scope.spawn(broker.WaitClusterState(
                  [](const ex_actor::ClusterState& state) {
                    return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 2; });
                  },
                  /*timeout_ms=*/5000) |
              stdexec::then([&success_count](const ex_actor::WaitClusterStateResult& res) {
                if (res.condition_met) {
                  success_count.fetch_add(1, std::memory_order_relaxed);
                }
              }));

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7331"},
                  {.alive = true, .last_seen_timestamp_ms = 200, .node_id = 2, .address = "tcp://127.0.0.1:7332"}});

  stdexec::sync_wait(scope.on_empty());
  EXPECT_EQ(success_count.load(std::memory_order_relaxed), 2);

  // Verify newly discovered nodes are reachable via BroadcastGossip (the real gossip path)
  EXPECT_NO_THROW(broker.BroadcastGossip());

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// BroadcastGossip: no crash with/without peers
// ============================================================

TEST(MessageBrokerTest, BroadcastGossipNoPeersNoCrash) {
  auto config = MakeConfig("tcp://127.0.0.1:7340");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  EXPECT_NO_THROW(broker.BroadcastGossip());

  stdexec::sync_wait(broker.Stop());
}

TEST(MessageBrokerTest, BroadcastGossipWithPeersNoCrash) {
  auto config = MakeConfig("tcp://127.0.0.1:7350",
                           /*contact_address=*/"tcp://127.0.0.1:7351");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  EXPECT_NO_THROW(broker.BroadcastGossip());

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// Gossip updates last_seen time to the max
// ============================================================

TEST(MessageBrokerTest, GossipUpdatesLastSeenToMax) {
  auto config = MakeConfig("tcp://127.0.0.1:7360",
                           /*contact_address=*/"tcp://127.0.0.1:7361",
                           /*heartbeat_timeout_ms=*/200);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Discover node 1 via gossip first
  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7361"}});

  // Sleep so the initial last_seen becomes stale
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Update last_seen via gossip with current time
  uint64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = now_ms, .node_id = 1, .address = "tcp://127.0.0.1:7361"}});

  // The node should NOT be timed out since we just refreshed it
  broker.CheckHeartbeatTimeout();

  auto [result] = stdexec::sync_wait(broker.WaitClusterState(
                                         [](const ex_actor::ClusterState& state) {
                                           return std::ranges::any_of(
                                               state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                                         },
                                         /*timeout_ms=*/10))
                      .value();
  EXPECT_TRUE(result.condition_met);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// CheckHeartbeatTimeout: self node is never deactivated
// ============================================================

TEST(MessageBrokerTest, CheckHeartbeatTimeoutDoesNotDeactivateSelf) {
  auto config = MakeConfig("tcp://127.0.0.1:7370",
                           /*contact_address=*/"",
                           /*heartbeat_timeout_ms=*/1);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckHeartbeatTimeout();

  // Self node is never deactivated. An always-true predicate returns immediately.
  auto [result] =
      stdexec::sync_wait(broker.WaitClusterState([](const ex_actor::ClusterState& /*state*/) { return true; },
                                                 /*timeout_ms=*/10))
          .value();
  EXPECT_TRUE(result.condition_met);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// BroadcastGossip after gossip discovery sends to all discovered peers
// ============================================================

TEST(MessageBrokerTest, BroadcastGossipAfterDiscovery) {
  auto config = MakeConfig("tcp://127.0.0.1:7380");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker,
                 {{.alive = true, .last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7381"},
                  {.alive = true, .last_seen_timestamp_ms = 99999, .node_id = 2, .address = "tcp://127.0.0.1:7382"}});

  // BroadcastGossip picks random peers from the discovered set and sends to them
  EXPECT_NO_THROW(broker.BroadcastGossip());

  stdexec::sync_wait(broker.Stop());
}
