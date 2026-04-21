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

namespace ex_actor::internal {
class MessageBrokerTestHelper {
 public:
  static void SetRequestHandler(MessageBroker& broker, MessageBroker::RequestHandler handler) {
    broker.request_handler_ = std::move(handler);
  }
};
}  // namespace ex_actor::internal

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

void DispatchIncomingRequest(ex_actor::internal::MessageBroker& broker, uint64_t request_node_id,
                             uint64_t response_node_id, uint64_t request_id, ByteBuffer payload) {
  ex_actor::internal::BrokerMessage broker_msg {.variant = ex_actor::internal::BrokerTwoWayMessage {
                                                    .request_node_id = request_node_id,
                                                    .response_node_id = response_node_id,
                                                    .request_id = request_id,
                                                    .payload = std::move(payload),
                                                }};
  auto raw = ex_actor::internal::Serialize(broker_msg);
  stdexec::sync_wait(broker.DispatchReceivedMessage(std::move(raw)));
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
              testing::Throws<ex_actor::NetworkError>(testing::Property(
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

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7221"}});

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

  stdexec::simple_counting_scope scope;
  std::atomic<bool> condition_met = true;

  stdexec::spawn(ex_actor::StartsInline(broker.WaitClusterState(
                     [](const ex_actor::ClusterState& state) {
                       return std::ranges::any_of(state.nodes,
                                                  [](const ex_actor::NodeInfo& n) { return n.node_id == 99; });
                     },
                     /*timeout_ms=*/0)) |
                     stdexec::then([&condition_met](const ex_actor::WaitClusterStateResult& res) {
                       condition_met.store(res.condition_met, std::memory_order_relaxed);
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckClusterStateWaiterTimeout();

  stdexec::sync_wait(scope.join());
  EXPECT_FALSE(condition_met.load(std::memory_order_relaxed));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// WaitClusterState resolves when gossip brings a new node
// ============================================================

TEST(MessageBrokerTest, WaitClusterStateResolvesOnGossipDiscovery) {
  auto config = MakeConfig("tcp://127.0.0.1:7240");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  stdexec::simple_counting_scope scope;
  std::atomic<bool> condition_met = false;

  stdexec::spawn(ex_actor::StartsInline(broker.WaitClusterState(
                     [](const ex_actor::ClusterState& state) {
                       return std::ranges::any_of(state.nodes,
                                                  [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                     },
                     /*timeout_ms=*/5000)) |
                     stdexec::then([&condition_met](const ex_actor::WaitClusterStateResult& res) {
                       condition_met.store(res.condition_met, std::memory_order_relaxed);
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7241"}});

  stdexec::sync_wait(scope.join());
  EXPECT_TRUE(condition_met.load(std::memory_order_relaxed));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// HandleGossipMessage: duplicate gossip for same node is idempotent
// ============================================================

TEST(MessageBrokerTest, DuplicateGossipForSameNodeDoesNotThrow) {
  auto config = MakeConfig("tcp://127.0.0.1:7250");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7251"}});
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 200, .node_id = 1, .address = "tcp://127.0.0.1:7251"}});

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// HandleGossipMessage: conflicting address for same node_id throws
// ============================================================

TEST(MessageBrokerTest, GossipWithConflictingAddressThrows) {
  auto config = MakeConfig("tcp://127.0.0.1:7260");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7261"}});

  EXPECT_THAT(
      [&]() {
        DispatchGossip(broker, {{.last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7262"}});
      },
      testing::Throws<std::exception>(
          testing::Property(&std::exception::what, testing::HasSubstr("Node 0x1 has conflicting address"))));

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// node_name: surfaced on self NodeInfo
// ============================================================

TEST(MessageBrokerTest, NodeNameFromConfigAppearsOnSelfNodeInfo) {
  auto config = MakeConfig("tcp://127.0.0.1:7265");
  config.node_name = "master";
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  auto [result] = stdexec::sync_wait(broker.WaitClusterState(
                                         [](const ex_actor::ClusterState& state) {
                                           return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) {
                                             return n.node_id == 0 && n.node_name == "master";
                                           });
                                         },
                                         /*timeout_ms=*/10))
                      .value();
  EXPECT_TRUE(result.condition_met);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// node_name: propagated via gossip onto peer NodeInfo
// ============================================================

TEST(MessageBrokerTest, NodeNameFromGossipAppearsOnPeerNodeInfo) {
  auto config = MakeConfig("tcp://127.0.0.1:7266");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999,
                           .node_id = 1,
                           .address = "tcp://127.0.0.1:7267",
                           .node_name = "worker-1"}});

  auto [result] = stdexec::sync_wait(broker.WaitClusterState(
                                         [](const ex_actor::ClusterState& state) {
                                           return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) {
                                             return n.node_id == 1 && n.node_name == "worker-1";
                                           });
                                         },
                                         /*timeout_ms=*/10))
                      .value();
  EXPECT_TRUE(result.condition_met);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// HandleGossipMessage: conflicting node_name for same node_id throws
// ============================================================

TEST(MessageBrokerTest, GossipWithConflictingNodeNameThrows) {
  auto config = MakeConfig("tcp://127.0.0.1:7268");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 100,
                           .node_id = 1,
                           .address = "tcp://127.0.0.1:7269",
                           .node_name = "worker-1"}});

  EXPECT_THAT(
      [&]() {
        DispatchGossip(broker, {{.last_seen_timestamp_ms = 100,
                                 .node_id = 1,
                                 .address = "tcp://127.0.0.1:7269",
                                 .node_name = "worker-2"}});
      },
      testing::Throws<std::exception>(
          testing::Property(&std::exception::what, testing::HasSubstr("Node 0x1 has conflicting node_name"))));

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
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7271"}});

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  // now node 1 should be dead
  broker.CheckHeartbeatTimeout();

  stdexec::simple_counting_scope scope;
  std::atomic<bool> condition_met = true;
  stdexec::spawn(
      ex_actor::StartsInline(broker.WaitClusterState(
          [](const ex_actor::ClusterState& state) {
            return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
          },
          /*timeout_ms=*/0)) |
          stdexec::then(
              [&condition_met](const ex_actor::WaitClusterStateResult& res) { condition_met = res.condition_met; }) |
          ex_actor::DiscardResult(),
      scope.get_token());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckClusterStateWaiterTimeout();

  stdexec::sync_wait(scope.join());
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
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7281"}});

  stdexec::simple_counting_scope scope;
  std::atomic<bool> got_error = false;

  stdexec::spawn(ex_actor::StartsInline(broker.SendRequest(/*to_node_id=*/1, MakeBytes("hello"))) |
                     stdexec::then([](const ByteBuffer&) {}) | stdexec::upon_error([&got_error](const auto&) noexcept {
                       got_error.store(true, std::memory_order_relaxed);
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckHeartbeatTimeout();

  stdexec::sync_wait(scope.join());
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
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7291"}});

  stdexec::simple_counting_scope scope;
  std::atomic<bool> got_response = false;
  std::string response_data;

  stdexec::spawn(ex_actor::StartsInline(broker.SendRequest(/*to_node_id=*/1, MakeBytes("ping"))) |
                     stdexec::then([&got_response, &response_data](const ByteBuffer& data) {
                       response_data = BytesToString(data);
                       got_response.store(true, std::memory_order_relaxed);
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  ex_actor::internal::BrokerMessage reply_msg {.variant = ex_actor::internal::BrokerTwoWayMessage {
                                                   .request_node_id = 0,
                                                   .response_node_id = 1,
                                                   .request_id = 0,
                                                   .payload = MakeBytes("pong"),
                                               }};
  auto raw = ex_actor::internal::Serialize(reply_msg);
  stdexec::sync_wait(broker.DispatchReceivedMessage(std::move(raw)));

  stdexec::sync_wait(scope.join());
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

  stdexec::simple_counting_scope scope;
  std::atomic<int> success_count = 0;

  for (int i = 0; i < 3; ++i) {
    stdexec::spawn(ex_actor::StartsInline(broker.WaitClusterState(
                       [](const ex_actor::ClusterState& state) {
                         return std::ranges::any_of(state.nodes,
                                                    [](const ex_actor::NodeInfo& n) { return n.node_id == 2; });
                       },
                       /*timeout_ms=*/5000)) |
                       stdexec::then([&success_count](const ex_actor::WaitClusterStateResult& res) {
                         if (res.condition_met) {
                           success_count.fetch_add(1, std::memory_order_relaxed);
                         }
                       }) |
                       ex_actor::DiscardResult(),
                   scope.get_token());
  }

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 2, .address = "tcp://127.0.0.1:7321"}});

  stdexec::sync_wait(scope.join());
  EXPECT_EQ(success_count.load(std::memory_order_relaxed), 3);

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// Multi-node gossip message introduces multiple nodes at once
// ============================================================

TEST(MessageBrokerTest, GossipIntroducesMultipleNodesAtOnce) {
  auto config = MakeConfig("tcp://127.0.0.1:7330");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  stdexec::simple_counting_scope scope;
  std::atomic<int> success_count = 0;

  stdexec::spawn(ex_actor::StartsInline(broker.WaitClusterState(
                     [](const ex_actor::ClusterState& state) {
                       return std::ranges::any_of(state.nodes,
                                                  [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                     },
                     /*timeout_ms=*/5000)) |
                     stdexec::then([&success_count](const ex_actor::WaitClusterStateResult& res) {
                       if (res.condition_met) {
                         success_count.fetch_add(1, std::memory_order_relaxed);
                       }
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  stdexec::spawn(ex_actor::StartsInline(broker.WaitClusterState(
                     [](const ex_actor::ClusterState& state) {
                       return std::ranges::any_of(state.nodes,
                                                  [](const ex_actor::NodeInfo& n) { return n.node_id == 2; });
                     },
                     /*timeout_ms=*/5000)) |
                     stdexec::then([&success_count](const ex_actor::WaitClusterStateResult& res) {
                       if (res.condition_met) {
                         success_count.fetch_add(1, std::memory_order_relaxed);
                       }
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 100, .node_id = 1, .address = "tcp://127.0.0.1:7331"},
                          {.last_seen_timestamp_ms = 200, .node_id = 2, .address = "tcp://127.0.0.1:7332"}});

  stdexec::sync_wait(scope.join());
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
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7361"}});

  // Sleep so the initial last_seen becomes stale
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Update last_seen via gossip with current time
  uint64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  DispatchGossip(broker, {{.last_seen_timestamp_ms = now_ms, .node_id = 1, .address = "tcp://127.0.0.1:7361"}});

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
// Timestamps use wall-clock (system_clock), not boot-relative (steady_clock).
// A remote node gossips its last_seen using wall-clock epoch time.
// If the local node measured time from a different epoch (e.g. steady_clock
// whose epoch is boot time), the heartbeat-timeout subtraction would produce
// a wildly wrong result and the node would be falsely declared dead.
// ============================================================

TEST(MessageBrokerTest, HeartbeatTimeoutUsesWallClockEpoch) {
  auto config = MakeConfig("tcp://127.0.0.1:7365",
                           /*contact_address=*/"tcp://127.0.0.1:7366",
                           /*heartbeat_timeout_ms=*/5000);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Simulate a remote node sending its last_seen in wall-clock (system_clock) epoch ms.
  // This value is astronomically larger than any steady_clock uptime value.
  uint64_t wall_clock_now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();

  DispatchGossip(broker,
                 {{.last_seen_timestamp_ms = wall_clock_now_ms, .node_id = 1, .address = "tcp://127.0.0.1:7366"}});

  // If the broker internally uses steady_clock, GetTimeMs() returns a small
  // uptime-based value.  The check `GetTimeMs() - last_seen_timestamp_ms`
  // would underflow (uint64_t) and be much larger than heartbeat_timeout_ms,
  // falsely killing the node.  With system_clock, the difference is ~0 ms so
  // the node stays alive.
  broker.CheckHeartbeatTimeout();

  stdexec::simple_counting_scope scope;
  std::atomic<bool> condition_met = false;
  stdexec::spawn(
      ex_actor::StartsInline(broker.WaitClusterState(
          [](const ex_actor::ClusterState& state) {
            return std::ranges::any_of(state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
          },
          /*timeout_ms=*/0)) |
          stdexec::then(
              [&condition_met](const ex_actor::WaitClusterStateResult& res) { condition_met = res.condition_met; }) |
          ex_actor::DiscardResult(),
      scope.get_token());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker.CheckClusterStateWaiterTimeout();

  stdexec::sync_wait(scope.join());
  EXPECT_TRUE(condition_met) << "Node 1 should still be alive; wall-clock timestamp was just set";

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// CheckHeartbeatTimeout: clock skew (future timestamp) must not kill node
// ============================================================

TEST(MessageBrokerTest, CheckHeartbeatTimeoutToleratesClockSkew) {
  auto config = MakeConfig("tcp://127.0.0.1:7367",
                           /*contact_address=*/"tcp://127.0.0.1:7368",
                           /*heartbeat_timeout_ms=*/100);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Simulate a remote node whose clock is slightly ahead of the local clock.
  // Its last_seen_timestamp_ms will be in the future relative to our GetTimeMs().
  // Without the clock-skew guard, the unsigned subtraction
  //   (now_ms - last_seen_timestamp_ms) would underflow to a huge value,
  // exceeding heartbeat_timeout_ms and falsely declaring the node dead.
  uint64_t future_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count() +
      5000;

  DispatchGossip(broker, {{.last_seen_timestamp_ms = future_ms, .node_id = 1, .address = "tcp://127.0.0.1:7368"}});

  // Even after sleeping past the heartbeat timeout, the future timestamp must
  // not cause an unsigned-underflow false positive.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  broker.CheckHeartbeatTimeout();

  // Node 1 should still be alive.
  auto [result] = stdexec::sync_wait(broker.WaitClusterState(
                                         [](const ex_actor::ClusterState& state) {
                                           return std::ranges::any_of(
                                               state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                                         },
                                         /*timeout_ms=*/10))
                      .value();
  EXPECT_TRUE(result.condition_met) << "Node with future timestamp should not be killed by heartbeat check";

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
// Contact node dies and restarts with same address but different node ID
// ============================================================

TEST(MessageBrokerTest, ContactNodeRestartWithSameAddressDifferentNodeId) {
  auto config = MakeConfig("tcp://127.0.0.1:7390",
                           /*contact_address=*/"tcp://127.0.0.1:7391",
                           /*heartbeat_timeout_ms=*/1);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  // Discover the contact node (node 1) via gossip
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7391"}});

  // Wait for the heartbeat to expire, then declare node 1 dead
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  broker.CheckHeartbeatTimeout();

  // The contact node restarts with the same address but a new node ID (node 2).
  // Before the fix, OnNodeAlive would crash here because the contact_node_send_socket_
  // had already been moved into node_id_to_send_socket_ for node 1.
  EXPECT_NO_THROW(
      DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 2, .address = "tcp://127.0.0.1:7391"}}));

  // The restarted node should be visible in the cluster state
  auto [result] = stdexec::sync_wait(broker.WaitClusterState(
                                         [](const ex_actor::ClusterState& state) {
                                           return std::ranges::any_of(
                                               state.nodes, [](const ex_actor::NodeInfo& n) { return n.node_id == 2; });
                                         },
                                         /*timeout_ms=*/10))
                      .value();
  EXPECT_TRUE(result.condition_met);

  // BroadcastGossip should also work without crashing
  EXPECT_NO_THROW(broker.BroadcastGossip());

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// Death is not propagated via gossip: a bad connection between 2 nodes
// must not cause other nodes to consider them dead
// ============================================================

TEST(MessageBrokerTest, DeadNodeIsNotBroadcastViaGossip) {
  // Node 0 knows about node 1 and node 2.
  // Node 1 times out from node 0's perspective, but node 2 should NOT learn
  // about node 1's death through gossip — each node decides independently.
  //
  // To exercise the real BroadcastGossip path, broker0 sends gossip over ZMQ
  // to a recv socket that feeds into broker2's DispatchReceivedMessage.
  //
  // Bind the capture socket first with a dynamic port so there is no chance of
  // "Address already in use" from a previous test's lingering ZMQ teardown.
  zmq::context_t capture_ctx {1};
  zmq::socket_t capture_socket {capture_ctx, zmq::socket_type::dealer};
  capture_socket.bind("tcp://127.0.0.1:*");
  capture_socket.set(zmq::sockopt::rcvtimeo, 2000);
  capture_socket.set(zmq::sockopt::linger, 0);
  std::string capture_endpoint = capture_socket.get(zmq::sockopt::last_endpoint);

  auto config0 = MakeConfig("tcp://127.0.0.1:7400",
                            /*contact_address=*/capture_endpoint,
                            /*heartbeat_timeout_ms=*/1);
  ex_actor::internal::MessageBroker broker0(/*this_node_id=*/0, config0);

  auto config2 = MakeConfig(capture_endpoint,
                            /*contact_address=*/"",
                            /*heartbeat_timeout_ms=*/60000);
  ex_actor::internal::MessageBroker broker2(/*this_node_id=*/2, config2);

  // Both brokers discover node 1
  DispatchGossip(broker0, {{.last_seen_timestamp_ms = 1, .node_id = 1, .address = "tcp://127.0.0.1:7401"},
                           {.last_seen_timestamp_ms = 99999, .node_id = 2, .address = capture_endpoint}});
  DispatchGossip(broker2, {{.last_seen_timestamp_ms = 99999, .node_id = 0, .address = "tcp://127.0.0.1:7400"},
                           {.last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7401"}});

  // Node 1 times out from broker0's perspective
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  broker0.CheckHeartbeatTimeout();

  // broker0 broadcasts gossip over ZMQ (contact node socket points to 7402)
  broker0.BroadcastGossip();

  // Receive the gossip message that broker0 actually sent
  zmq::message_t captured_msg;
  auto recv_result = capture_socket.recv(captured_msg);
  ASSERT_TRUE(recv_result.has_value()) << "Expected to receive a gossip message from broker0";

  // Feed the captured raw gossip into broker2
  ex_actor::internal::ByteBuffer raw(static_cast<const std::byte*>(captured_msg.data()),
                                     static_cast<const std::byte*>(captured_msg.data()) + captured_msg.size());
  stdexec::sync_wait(broker2.DispatchReceivedMessage(std::move(raw)));

  // broker2 should still see node 1 as alive — the death was not propagated.
  // Use async_scope + manual timeout check to avoid blocking forever if the
  // bug is present (node 1 would be dead and the predicate never satisfied).
  stdexec::simple_counting_scope scope;
  std::atomic<bool> node1_alive = false;
  stdexec::spawn(ex_actor::StartsInline(broker2.WaitClusterState(
                     [](const ex_actor::ClusterState& state) {
                       return std::ranges::any_of(state.nodes,
                                                  [](const ex_actor::NodeInfo& n) { return n.node_id == 1; });
                     },
                     /*timeout_ms=*/0)) |
                     stdexec::then([&node1_alive](const ex_actor::WaitClusterStateResult& res) {
                       node1_alive.store(res.condition_met, std::memory_order_relaxed);
                     }) |
                     ex_actor::DiscardResult(),
                 scope.get_token());

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  broker2.CheckClusterStateWaiterTimeout();
  stdexec::sync_wait(scope.join());

  EXPECT_TRUE(node1_alive.load(std::memory_order_relaxed)) << "Node 2 should still see node 1 as alive; "
                                                              "only the node-0-to-node-1 connection was bad";

  stdexec::sync_wait(broker0.Stop());
  stdexec::sync_wait(broker2.Stop());
  capture_socket.close();
  capture_ctx.close();
}

// ============================================================
// BroadcastGossip after gossip discovery sends to all discovered peers
// ============================================================

TEST(MessageBrokerTest, BroadcastGossipAfterDiscovery) {
  auto config = MakeConfig("tcp://127.0.0.1:7380");
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/0, config);

  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 1, .address = "tcp://127.0.0.1:7381"},
                          {.last_seen_timestamp_ms = 99999, .node_id = 2, .address = "tcp://127.0.0.1:7382"}});

  // BroadcastGossip picks random peers from the discovered set and sends to them
  EXPECT_NO_THROW(broker.BroadcastGossip());

  stdexec::sync_wait(broker.Stop());
}

// ============================================================
// Deferred replies survive node connection loss:
// When a reply is deferred (because we have no send socket for the requester),
// and the requester node dies and reconnects, the deferred reply must still be
// flushed upon reconnection.
// ============================================================

TEST(MessageBrokerTest, DeferredReplySurvivesNodeConnectionLossAndReconnection) {
  // Bind the capture socket first with a dynamic port so there is no chance of
  // "Address already in use" from a previous test's lingering ZMQ teardown.
  zmq::context_t capture_ctx {1};
  zmq::socket_t capture_socket {capture_ctx, zmq::socket_type::dealer};
  capture_socket.bind("tcp://127.0.0.1:*");
  capture_socket.set(zmq::sockopt::rcvtimeo, 2000);
  capture_socket.set(zmq::sockopt::linger, 0);
  std::string capture_endpoint = capture_socket.get(zmq::sockopt::last_endpoint);

  // Broker B (node 1) will receive requests from node 0 and defer replies.
  auto config = MakeConfig("tcp://127.0.0.1:7410",
                           /*contact_address=*/"",
                           /*heartbeat_timeout_ms=*/1);
  ex_actor::internal::MessageBroker broker(/*this_node_id=*/1, config);

  // Set up an echo request handler so HandleIncomingRequest can process requests.
  ex_actor::internal::MessageBrokerTestHelper::SetRequestHandler(
      broker, [](ByteBuffer data) -> stdexec::task<ByteBuffer> { co_return std::move(data); });

  // --- Phase 1: Request arrives while node 0 is unknown → reply deferred ---
  // Node 0 sends a request to broker (node 1). Broker doesn't know node 0 yet,
  // so the reply will be deferred in deferred_replies_.
  DispatchIncomingRequest(broker, /*request_node_id=*/0, /*response_node_id=*/1,
                          /*request_id=*/100, MakeBytes("request_1"));

  // --- Phase 2: Node 0 appears via gossip (with stale timestamp) ---
  // OnNodeAlive is triggered → send socket created → deferred reply flushed.
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 1, .node_id = 0, .address = capture_endpoint}});

  // Capture socket should receive the flushed reply.
  {
    zmq::message_t reply_msg;
    auto recv_result = capture_socket.recv(reply_msg);
    ASSERT_TRUE(recv_result.has_value()) << "Expected to receive the flushed deferred reply (phase 2)";
    auto reply_broker_msg = ex_actor::internal::Deserialize<ex_actor::internal::BrokerMessage>(
        ex_actor::internal::ByteBuffer(static_cast<const std::byte*>(reply_msg.data()),
                                       static_cast<const std::byte*>(reply_msg.data()) + reply_msg.size()));
    auto& two_way = std::get<ex_actor::internal::BrokerTwoWayMessage>(reply_broker_msg.variant);
    EXPECT_EQ(two_way.request_node_id, 0U);
    EXPECT_EQ(two_way.request_id, 100U);
    EXPECT_EQ(BytesToString(two_way.payload), "request_1");
  }

  // --- Phase 3: Node 0 dies (heartbeat timeout) ---
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  broker.CheckHeartbeatTimeout();

  // --- Phase 4: While node 0 is dead, another request arrives → reply deferred ---
  DispatchIncomingRequest(broker, /*request_node_id=*/0, /*response_node_id=*/1,
                          /*request_id=*/200, MakeBytes("request_2"));

  // --- Phase 5: Node 0 reappears via gossip ---
  // Because the fix removes node 0 from node_id_to_state_ on death (instead of
  // marking alive=false), this gossip triggers OnNodeAlive again, which flushes
  // the deferred reply accumulated in phase 4.
  DispatchGossip(broker, {{.last_seen_timestamp_ms = 99999, .node_id = 0, .address = capture_endpoint}});

  // Capture socket should receive the second deferred reply.
  {
    zmq::message_t reply_msg;
    auto recv_result = capture_socket.recv(reply_msg);
    ASSERT_TRUE(recv_result.has_value()) << "Expected to receive the flushed deferred reply (phase 5)";
    auto reply_broker_msg = ex_actor::internal::Deserialize<ex_actor::internal::BrokerMessage>(
        ex_actor::internal::ByteBuffer(static_cast<const std::byte*>(reply_msg.data()),
                                       static_cast<const std::byte*>(reply_msg.data()) + reply_msg.size()));
    auto& two_way = std::get<ex_actor::internal::BrokerTwoWayMessage>(reply_broker_msg.variant);
    EXPECT_EQ(two_way.request_node_id, 0U);
    EXPECT_EQ(two_way.request_id, 200U);
    EXPECT_EQ(BytesToString(two_way.payload), "request_2");
  }

  stdexec::sync_wait(broker.Stop());
  capture_socket.close();
  capture_ctx.close();
}
