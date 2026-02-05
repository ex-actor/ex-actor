#include "ex_actor/internal/network.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "ex_actor/api.h"

using ex_actor::internal::network::ByteBufferType;
using ex_actor::internal::network::NodeInfoManager;
using Liveness = NodeInfoManager::Liveness;
namespace logging = ex_actor::internal::logging;

TEST(NetworkTest, NodeInfoManagerBasicTest) {
  NodeInfoManager manager {0};

  manager.Add(1, {.liveness = Liveness::kAlive, .last_seen = 100, .address = "tcp://127.0.0.1:7101"});
  manager.Add(2, {.liveness = Liveness::kAlive, .last_seen = 200, .address = "tcp://127.0.0.1:7102"});

  EXPECT_TRUE(manager.Contains(1));
  EXPECT_TRUE(manager.Connected(1));
  EXPECT_TRUE(manager.Connected(2));
  EXPECT_FALSE(manager.Contains(3));

  manager.RefreshLastSeen(1, 50);
  manager.RefreshLastSeen(1, 150);
  manager.RefreshLastSeen(3, 300);

  auto gossip_messages = manager.GenerateGossipMessage();
  auto find_gossip = [&](uint32_t node_id) {
    return std::ranges::find_if(gossip_messages, [node_id](const ex_actor::internal::network::GossipMessage& message) {
      return message.node_info.node_id == node_id;
    });
  };
  auto node_1 = find_gossip(1);
  auto node_2 = find_gossip(2);
  EXPECT_NE(node_1, gossip_messages.end());
  EXPECT_NE(node_2, gossip_messages.end());
  EXPECT_EQ(node_1->last_seen, 150U);
  EXPECT_EQ(node_2->last_seen, 200U);
  EXPECT_EQ(node_1->node_info.address, "tcp://127.0.0.1:7101");
  EXPECT_EQ(node_2->node_info.address, "tcp://127.0.0.1:7102");
  EXPECT_EQ(find_gossip(3), gossip_messages.end());

  auto peers = manager.GetRandomPeers(10);
  for (const auto& node : peers) {
    EXPECT_TRUE(node.node_id == 1 || node.node_id == 2);
  }

  auto [alive] = stdexec::sync_wait(manager.WaitNodeAlive(1, std::chrono::milliseconds {10})).value();
  EXPECT_TRUE(alive);

  manager.DeactivateNode(1);
  manager.DeactivateNode(2);
  EXPECT_TRUE(manager.Connected(1));
  EXPECT_TRUE(manager.Connected(2));
}

TEST(NetworkTest, NodeInfoManagerWaiterTest) {
  NodeInfoManager manager {0};
  std::atomic<int> woke_count = 0;
  exec::async_scope waiter_scope;
  std::atomic_bool done = false;

  auto spawn_waiter = [&](uint32_t node_id) {
    waiter_scope.spawn(manager.WaitNodeAlive(node_id, std::chrono::milliseconds {1000}) |
                       stdexec::then([&woke_count](bool result) {
                         if (result) {
                           woke_count.fetch_add(1, std::memory_order_relaxed);
                         }
                       }));
  };

  spawn_waiter(4);
  spawn_waiter(4);

  std::jthread notifier([&]() {
    manager.NotifyWaiters(4);
    std::this_thread::sleep_for(std::chrono::milliseconds {1});
  });

  stdexec::sync_wait(waiter_scope.on_empty());

  EXPECT_EQ(woke_count.load(std::memory_order_relaxed), 2);
}

TEST(NetworkTest, MessageBrokerBasicTest) {
  auto test_once = []() {
    std::vector<ex_actor::NodeInfo> node_list = {{.node_id = 0, .address = "tcp://127.0.0.1:5201"},
                                                 {.node_id = 1, .address = "tcp://127.0.0.1:5202"},
                                                 {.node_id = 2, .address = "tcp://127.0.0.1:5203"}};

    auto node_main = [](const std::vector<ex_actor::NodeInfo>& node_list, uint32_t node_id) {
      ex_actor::internal::util::SetThreadName("node_" + std::to_string(node_id));
      ex_actor::internal::network::MessageBroker message_broker(
          node_list,
          /*this_node_id=*/node_id,
          [&message_broker](uint64_t received_request_id, ByteBufferType data) {
            message_broker.ReplyRequest(received_request_id, std::move(data));
          },
          ex_actor::NetworkConfig {.gossip_interval = std::chrono::milliseconds {50}});
      uint32_t to_node_id = (node_id + 1) % node_list.size();
      // Waiting all the nodes find its contact node
      std::this_thread::sleep_for(std::chrono::milliseconds {500});
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

TEST(NetworkTest, MessageBrokerClusterConfigTest) {
  std::vector<ex_actor::NodeInfo> node_list = {{.node_id = 0, .address = "tcp://127.0.0.1:6201"},
                                               {.node_id = 1, .address = "tcp://127.0.0.1:6202"},
                                               {.node_id = 2, .address = "tcp://127.0.0.1:6203"}};
  auto test_once = [&node_list]() {
    auto node_main = [](const std::vector<ex_actor::NodeInfo>& node_list, uint32_t node_id) {
      ex_actor::internal::util::SetThreadName("node_" + std::to_string(node_id));
      ex_actor::ClusterConfig config {.network_config {.gossip_interval = std::chrono::milliseconds {50}}};
      config.this_node = node_list.at(node_id);
      if (node_id != 0) {
        config.contact_node = node_list.front();
      }
      ex_actor::internal::network::MessageBroker message_broker(
          config, [&message_broker](uint64_t received_request_id, ByteBufferType data) {
            message_broker.ReplyRequest(received_request_id, std::move(data));
          });
      uint32_t to_node_id = (node_id + 1) % node_list.size();
      // Waiting all the nodes find its contact node
      std::this_thread::sleep_for(std::chrono::milliseconds {500});
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
