// Copyright 2025 The ex_actor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <oneapi/tbb/partitioner.h>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "ex_actor/internal/constants.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {
struct NodeInfo {
  uint32_t node_id = 0;
  std::string address;
  friend bool operator==(const NodeInfo& lhs, const NodeInfo& rhs) { return lhs.node_id == rhs.node_id; }
};
}  // namespace ex_actor

namespace std {
template <>
struct hash<ex_actor::NodeInfo> {
  size_t operator()(const ex_actor::NodeInfo& k) const noexcept { return hash<uint32_t> {}(k.node_id); }
};
}  // namespace std

namespace ex_actor::internal::network {
using ByteBufferType = zmq::message_t;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

enum class MessageFlag : uint8_t { kNormal = 0, kQuit, kHeartbeat, kGossip };

struct Identifier {
  uint32_t request_node_id;
  uint32_t response_node_id;
  uint64_t request_id_in_node;
  MessageFlag flag;
};

struct NetworkConfig {
  std::chrono::milliseconds heartbeat_timeout = kDefaultHeartbeatTimeout;
  std::chrono::milliseconds gossip_interval = kDefaultGossipInterval;
};

struct ClusterConfig {
  NodeInfo this_node;
  NodeInfo contact_node = {};
  network::NetworkConfig network_config = {};
};

struct GossipMessage {
  NodeInfo node_info;
  TimePoint last_seen;
};

class PeerNodes {
 public:
  enum class Liveness : uint8_t { kAlive = 0, kQuitting, kDead };
  struct NodeState {
    Liveness liveness;
    TimePoint last_seen;
  };
  void Add(const NodeInfo& node, const NodeState& state) {
    std::lock_guard lock(mutex_);
    map_.try_emplace(node, state);
    alive_peers_ += 1;
  }

  void RefreshLastSeen(const uint32_t& node_id, TimePoint last_seen) {
    std::lock_guard lock(mutex_);
    auto& state = map_.at({.node_id = node_id});
    state.last_seen = max(last_seen, state.last_seen);
  }

  void CheckHeartbeat(const std::chrono::milliseconds& timeout) {
    std::lock_guard lock(mutex_);
    for (const auto& pair : map_) {
      const auto& state = pair.second;
      if (state.liveness != Liveness::kDead && std::chrono::steady_clock::now() - state.last_seen >= timeout) {
        logging::Error("Node {} is dead, try to exit", pair.first.node_id);
        // don't call static variables' destructors, or the program will hang in MessageBroker's destructor
        std::quick_exit(1);
      }
    }
  }

  bool Contains(const uint32_t& node_id) {
    std::lock_guard lock(mutex_);
    return map_.contains({.node_id = node_id});
  }

  void DeactivateNode(const uint32_t& node_id) {
    NodeInfo node {.node_id = node_id};
    std::lock_guard lock(mutex_);
    auto& state = map_.at(node);
    if (state.liveness == Liveness::kAlive) {
      state.liveness = Liveness::kQuitting;
      alive_peers_ -= 1;
    }

    if (alive_peers_ == 0) {
      cv_.notify_all();
    }
  }

  void WaitAllNodesExit() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this]() { return alive_peers_ == 0; });
  }

  std::vector<NodeInfo> GetNodeList() {
    std::vector<NodeInfo> node_list {};
    node_list.reserve(map_.size());
    std::lock_guard lock(mutex_);
    for (const auto& pair : map_) {
      const auto& node = pair.first;
      const auto& liveness = pair.second.liveness;
      if (liveness != Liveness::kDead) {
        node_list.push_back(pair.first);
      }
    }
    return node_list;
  }

  std::vector<GossipMessage> GenerateGossipMessages() {
    std::vector<GossipMessage> messages;
    messages.reserve(map_.size());
    std::lock_guard lock(mutex_);
    for (const auto& pair : map_) {
      messages.push_back({.node_info = pair.first, .last_seen = pair.second.last_seen});
    }
    return messages;
  }

  std::vector<NodeInfo> GetRandomPeers(size_t size) {
    std::vector<NodeInfo> nodes;
    if (size == 0) {
      return nodes;
    }

    {
      std::lock_guard lock(mutex_);
      nodes.reserve(map_.size());
      size_t seen = 0;
      for (const auto& pair : map_) {
        const NodeInfo& node = pair.first;
        const NodeState& state = pair.second;
        if (state.liveness == Liveness::kDead) {
          continue;
        }
        nodes.push_back(node);
      }
    }

    // NOTE: We only call this method in the send_thread, so we don't need to hold lock here.
    std::shuffle(nodes.begin(), nodes.end(), rng_);
    nodes.resize(std::min(nodes.size(), size));
    return nodes;
  }

 private:
  std::unordered_map<NodeInfo, NodeState> map_;
  std::condition_variable cv_;
  std::mutex mutex_;
  std::mt19937 rng_ {std::random_device {}()};
  uint32_t alive_peers_ = 0;
};

class MessageBroker {
 public:
  explicit MessageBroker(std::vector<NodeInfo> node_list, uint32_t this_node_id,
                         std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler,
                         NetworkConfig network_config = {});
  explicit MessageBroker(const ClusterConfig& cluster_config,
                         std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler);

  ~MessageBroker();

  void ClusterAlignedStop();

  // -------- std::execution sender adaption start--------
  struct TypeErasedSendOperation {
    virtual ~TypeErasedSendOperation() = default;
    virtual void Complete(ByteBufferType /*response_data*/) {
      EXA_THROW << "TypeErasedOperation::Complete should not be called";
    }
    TypeErasedSendOperation(Identifier identifier, ByteBufferType data, MessageBroker* message_broker)
        : identifier(identifier), data(std::move(data)), message_broker(message_broker) {}
    Identifier identifier;
    ByteBufferType data;
    MessageBroker* message_broker {};
  };
  template <ex::receiver R>
  struct SendRequestOperation : TypeErasedSendOperation {
    SendRequestOperation(Identifier identifier, ByteBufferType data, MessageBroker* message_broker, R receiver)
        : TypeErasedSendOperation(identifier, std::move(data), message_broker), receiver(std::move(receiver)) {}
    R receiver;
    std::atomic_bool started = false;
    void start() noexcept {
      bool expected = false;
      bool changed = started.compare_exchange_strong(expected, true);
      if (!changed) [[unlikely]] {
        logging::Critical("MessageBroker Operation already started");
        std::terminate();
      }
      message_broker->PushOperation(this);
    }
    void Complete(ByteBufferType response_data) override { receiver.set_value(std::move(response_data)); }
  };
  struct SendRequestSender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t(ByteBufferType)>;
    Identifier identifier;
    ByteBufferType data;
    MessageBroker* message_broker;
    template <ex::receiver R>
    SendRequestOperation<R> connect(R receiver) {
      return SendRequestOperation<R>(identifier, std::move(data), message_broker, std::move(receiver));
    }
  };
  // -------- std::execution sender adaption end--------

  /**
   * @brief Send buffer to the remote node.
   * @return A sender containing raw response buffer.
   */
  SendRequestSender SendRequest(uint32_t to_node_id, ByteBufferType data, MessageFlag flag = MessageFlag::kNormal);

  void ReplyRequest(uint64_t received_request_id, ByteBufferType data);

  bool CheckNode(uint32_t node_id);

 private:
  void EstablishConnectionTo(const NodeInfo& node_info);
  void EstablishConnections(const std::vector<NodeInfo>& node_list);
  void PushOperation(TypeErasedSendOperation* operation);
  void SendProcessLoop(const std::stop_token& stop_token);
  void ReceiveProcessLoop(const std::stop_token& stop_token);
  void HandleReceivedMessage(zmq::multipart_t multi);
  void SendHeartbeat(const std::vector<NodeInfo>& node_list);
  void SendGossip(const std::vector<NodeInfo>& node_list);
  void CheckHeartbeat();
  void SendHeartbeatOrGossip();
  void HandleGossip(zmq::message_t gossip_msg);

  struct ReplyOperation {
    Identifier identifier;
    ByteBufferType data;
  };

  uint32_t this_node_id_;
  NodeInfo this_node_ {};
  std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler_;
  NetworkConfig network_config_;
  std::atomic_uint64_t send_request_id_counter_ = 0;
  std::atomic_uint64_t received_request_id_counter_ = 0;
  size_t contact_node_index_ = 0;

  zmq::context_t context_ {/*io_threads_=*/1};
  util::LockGuardedMap<uint32_t, zmq::socket_t> node_id_to_send_socket_;
  zmq::socket_t recv_socket_ {context_, zmq::socket_type::dealer};

  util::LinearizableUnboundedMpscQueue<TypeErasedSendOperation*> pending_send_operations_;
  util::LockGuardedMap<uint64_t, TypeErasedSendOperation*> send_request_id_to_operation_;
  util::LinearizableUnboundedMpscQueue<ReplyOperation> pending_reply_operations_;
  util::LockGuardedMap<uint64_t, Identifier> received_request_id_to_identifier_;

  std::jthread send_thread_;
  std::jthread recv_thread_;
  std::atomic_bool stopped_ = false;
  PeerNodes peer_nodes_;
  exec::async_scope async_scope_;
  bool enable_dynamic_connectivity_ = false;

  TimePoint last_heartbeat_;
};

}  // namespace ex_actor::internal::network

namespace ex_actor {
using internal::network::ClusterConfig;
using internal::network::NetworkConfig;
}  // namespace ex_actor
