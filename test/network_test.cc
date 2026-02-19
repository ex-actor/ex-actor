#include "ex_actor/internal/network.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "ex_actor/api.h"

using ex_actor::internal::ByteBufferType;
using ex_actor::internal::NodeInfoManager;
using Liveness = ex_actor::internal::NodeState::Liveness;
namespace logging = ex_actor::internal::log;

TEST(NetworkTest, NodeInfoManagerBasicTest) {
  NodeInfoManager manager {0};

  manager.Add(1, {.liveness = Liveness::kAlive, .last_seen = 100, .node_id = 1, .address = "tcp://127.0.0.1:7101"});
  manager.Add(2, {.liveness = Liveness::kAlive, .last_seen = 200, .node_id = 2, .address = "tcp://127.0.0.1:7102"});

  EXPECT_TRUE(manager.Contains(1));
  EXPECT_TRUE(manager.Connected(1));
  EXPECT_TRUE(manager.Connected(2));
  EXPECT_FALSE(manager.Contains(3));

  manager.RefreshLastSeen(1, 50);
  manager.RefreshLastSeen(1, 150);
  manager.RefreshLastSeen(3, 300);

  auto gossip_message = manager.GenerateGossipMessage();
  const auto& node_states = gossip_message.node_states;
  auto find_gossip = [&](uint32_t node_id) {
    return std::ranges::find_if(
        node_states, [node_id](const ex_actor::internal::NodeState& state) { return state.node_id == node_id; });
  };
  auto node_1 = find_gossip(1);
  auto node_2 = find_gossip(2);
  EXPECT_NE(node_1, node_states.end());
  EXPECT_NE(node_2, node_states.end());
  EXPECT_EQ(node_1->last_seen, 150U);
  EXPECT_EQ(node_2->last_seen, 200U);
  EXPECT_EQ(node_1->address, "tcp://127.0.0.1:7101");
  EXPECT_EQ(node_2->address, "tcp://127.0.0.1:7102");
  EXPECT_EQ(find_gossip(3), node_states.end());

  auto peers = manager.GetRandomPeers(10);
  for (const auto& node : peers) {
    EXPECT_TRUE(node.node_id == 1 || node.node_id == 2);
  }

  auto [alive] = stdexec::sync_wait(manager.WaitNodeAlive(1, 10)).value();
  EXPECT_TRUE(alive);

  manager.DeactivateNode(1);
  manager.DeactivateNode(2);
  EXPECT_TRUE(manager.Connected(1));
  EXPECT_TRUE(manager.Connected(2));
  EXPECT_TRUE(manager.GenerateGossipMessage().node_states.empty());

  manager.Add(4, {.liveness = Liveness::kAlive, .last_seen = 100, .node_id = 4, .address = "tcp://127.0.0.1:7301"});

  EXPECT_THAT([&manager]() -> void { (void)manager.Connected(4, "tcp://127.0.0.1:7302"); },
              testing::Throws<std::exception>(testing::Property(
                  &std::exception::what,
                  testing::HasSubstr("Nodes with the same node ID but different addresses exist in the cluster."))));
}

TEST(NetworkTest, NodeInfoManagerWaiterTest) {
  NodeInfoManager manager {0};
  std::atomic<int> woke_count = 0;
  exec::async_scope waiter_scope;
  std::atomic_bool done = false;

  auto spawn_waiter = [&](uint32_t node_id) {
    waiter_scope.spawn(manager.WaitNodeAlive(node_id, 1000) | stdexec::then([&woke_count](bool result) {
                         if (result) {
                           woke_count.fetch_add(1, std::memory_order_relaxed);
                         }
                       }));
  };

  spawn_waiter(4);
  spawn_waiter(4);

  manager.NotifyWaiters(4);
  stdexec::sync_wait(waiter_scope.on_empty());

  EXPECT_EQ(woke_count.load(std::memory_order_relaxed), 2);
}

TEST(NetworkTest, MessageBrokerClusterConfigTest) {
  std::vector<ex_actor::NodeInfo> node_list = {{.node_id = 0, .address = "tcp://127.0.0.1:6201"},
                                               {.node_id = 1, .address = "tcp://127.0.0.1:6202"},
                                               {.node_id = 2, .address = "tcp://127.0.0.1:6203"}};
  auto test_once = [&node_list]() {
    std::barrier bar {3};
    auto node_main = [&bar](const std::vector<ex_actor::NodeInfo>& node_list, uint32_t node_id) {
      ex_actor::internal::SetThreadName("node_" + std::to_string(node_id));
      ex_actor::ClusterConfig config {.network_config {.gossip_interval_ms = 50}};
      config.this_node = node_list.at(node_id);
      if (node_id != 0) {
        config.contact_node = node_list.front();
      }
      ex_actor::internal::MessageBroker message_broker(
          config, [&message_broker](uint64_t received_request_id, ByteBufferType data) {
            message_broker.ReplyRequest(received_request_id, std::move(data));
          });
      uint32_t to_node_id = (node_id + 1) % node_list.size();
      // Waiting all the nodes find its contact node
      stdexec::sync_wait(message_broker.WaitNodeAlive(to_node_id, 500));
      exec::async_scope scope;
      for (int i = 0; i < 5; ++i) {
        scope.spawn(message_broker.SendRequest(to_node_id, ByteBufferType(std::to_string(node_id))) |
                    stdexec::then([node_id](ByteBufferType data) {
                      std::string data_str(static_cast<char*>(data.data()), data.size());
                      logging::Info("got response data, node id: {}, data: {}", node_id, data_str);
                      ASSERT_EQ(data_str, std::to_string(node_id));
                    }));
      }
      stdexec::sync_wait(scope.on_empty());
      bar.arrive_and_wait();
    };

    std::jthread node_0(node_main, node_list, 0);
    std::jthread node_1(node_main, node_list, 1);
    std::jthread node_2(node_main, node_list, 2);
  };

  for (int i = 0; i < 10; ++i) {
    logging::Info("test once, {}", i);
    test_once();
  }
}

TEST(NetworkTest, MessageBrokerDuplicateContactNodeTest) {
  auto request_handler = [](uint64_t /*received_request_id*/, ByteBufferType /*data*/) {};

  auto make_cluster_config = [](std::string this_address, std::string contact_address) {
    ex_actor::ClusterConfig config;
    config.this_node = {.node_id = 0, .address = std::move(this_address)};
    config.contact_node = {.node_id = 0, .address = std::move(contact_address)};
    return config;
  };

  auto duplicate_node_id_and_addr = make_cluster_config("tcp://127.0.0.1:7201", "tcp://127.0.0.1:7201");
  EXPECT_THAT([&]() -> void { (void)ex_actor::internal::MessageBroker(duplicate_node_id_and_addr, request_handler); },
              testing::Throws<std::exception>(testing::Property(
                  &std::exception::what,
                  testing::HasSubstr("The local node has the same node ID or address as the contact node."))));

  auto duplicate_node_id_different_addr = make_cluster_config("tcp://127.0.0.1:7202", "tcp://127.0.0.1:7203");
  EXPECT_THAT(
      [&]() -> void { (void)ex_actor::internal::MessageBroker(duplicate_node_id_different_addr, request_handler); },
      testing::Throws<std::exception>(testing::Property(
          &std::exception::what, testing::HasSubstr("The local node has the same node ID or address as the "
                                                    "contact node."))));
}

TEST(NetworkTest, MessageBrokerDuplicateClusterNodesTest) {
  auto request_handler = [](uint64_t /*received_request_id*/, ByteBufferType /*data*/) {};

  std::vector<ex_actor::NodeInfo> nodes {ex_actor::NodeInfo {.node_id = 0, .address = "tcp://127.0.0.1:5000"},
                                         ex_actor::NodeInfo {.node_id = 2, .address = "tcp://127.0.0.1:5001"},
                                         ex_actor::NodeInfo {.node_id = 2, .address = "tcp://127.0.0.1:5002"},
                                         ex_actor::NodeInfo {.node_id = 0, .address = "tcp://127.0.0.1:5003"}};

  auto create_node = [&request_handler](ex_actor::NodeInfo this_node, ex_actor::NodeInfo contact_node = {}) {
    ex_actor::ClusterConfig cluster_config {.this_node = std::move(this_node), .contact_node = std::move(contact_node)};
    ex_actor::internal::MessageBroker message_broker(cluster_config, request_handler);
    std::this_thread::sleep_for(std::chrono::milliseconds {1000});
  };

  auto duplicate_node_test = [&]() {
    std::jthread node_0(create_node, nodes.at(0));
    std::jthread node_1(create_node, nodes.at(1), nodes.at(0));
    std::jthread node_2(create_node, nodes.at(2), nodes.at(0));
  };

#ifdef _WIN32
  EXPECT_DEATH(duplicate_node_test(), "");
#else
  EXPECT_DEATH(duplicate_node_test(), "Nodes with the same node ID but different addresses exist in the cluster");
#endif  // _WIN32

  auto duplicate_node_forward_test = [&]() {
    std::jthread node_0(create_node, nodes.at(0));
    std::jthread node_1(create_node, nodes.at(1), nodes.at(0));
    // Let the node 1 forward this duplicate node to node 0
    std::jthread node_2(create_node, nodes.at(3), nodes.at(1));
  };

#ifdef _WIN32
  EXPECT_DEATH(duplicate_node_forward_test(), "");
#else
  EXPECT_DEATH(duplicate_node_forward_test(),
               "Nodes with the same node ID but different addresses exist in the cluster");
#endif  // _WIN32
}
