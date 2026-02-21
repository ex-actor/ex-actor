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
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "ex_actor/internal/constants.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {
struct NodeInfo {
  /// Unique ID for this node, should be unique within the cluster.
  uint32_t node_id = 0;
  /// Format: "<protocol>://<IP>:<port>". For the current node, we'll open a listener on this address. For other nodes,
  /// we'll connect to this address.
  std::string address;
};

struct NetworkConfig {
  /// How long (ms) should we consider a node dead if we haven't received any messages from it
  uint64_t heartbeat_timeout_ms = internal::kDefaultHeartbeatTimeoutMs;
  /// Interval (ms) at which gossip messages are sent.
  uint64_t gossip_interval_ms = internal::kDefaultGossipIntervalMs;
  /// Number of peers to propagate each gossip message to per round.
  size_t gossip_fanout = internal::kDefaultGossipFanout;
};

struct ClusterConfig {
  NodeInfo this_node;
  /// If you are the first node in the cluster, leave it empty. Otherwise, set it to any other node in the cluster.
  NodeInfo contact_node;
  NetworkConfig network_config;
};
}  // namespace ex_actor

namespace ex_actor::internal {
using ByteBufferType = zmq::message_t;

enum class MessageFlag : uint8_t { kNormal = 0, kQuit, kGossip };

struct Identifier {
  uint32_t request_node_id;
  uint32_t response_node_id;
  uint64_t request_id_in_node;
  MessageFlag flag;
};

struct Waiter {
  explicit Waiter(uint64_t deadline_ms) : sem(1), deadline_ms(deadline_ms) {}
  ex_actor::Semaphore sem;
  uint64_t deadline_ms;
  std::atomic_bool arrive = false;
};

class NodeInfoManager {
 public:
  explicit NodeInfoManager(uint32_t this_node_id);
  void Add(uint32_t node_id, const NodeState& state);
  void RefreshLastSeen(uint32_t node_id, uint64_t last_seen);
  bool Connected(uint32_t node_id, const std::string& address = "");
  bool Contains(uint32_t node_id);
  void DeactivateNode(uint32_t node_id);
  void WaitAllNodesExit();
  std::vector<NodeInfo> GetHealthyNodeList();
  void PrintAllNodesState(uint32_t this_node_id);
  GossipMessage GenerateGossipMessage();
  std::vector<NodeInfo> GetRandomPeers(size_t size);
  exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms);
  void NotifyWaiters(uint32_t node_id);
  void CheckHeartbeatAndExpireWaiters(uint64_t timeout_ms);

 private:
  uint32_t this_node_id_;
  std::unordered_map<uint32_t, NodeState> node_id_to_state_;
  std::unordered_map<uint32_t, std::vector<std::shared_ptr<Waiter>>> node_id_to_waiters_;
  std::condition_variable cv_;
  std::mutex mutex_;
  std::mt19937 rng_ {std::random_device {}()};
  uint32_t alive_peers_ = 0;
};

class MessageBroker {
 public:
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
        log::Critical("MessageBroker Operation already started");
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

  bool CheckNodeConnected(uint32_t node_id);

  exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms);

 private:
  void EstablishConnectionTo(const NodeInfo& node_info);
  void PushOperation(TypeErasedSendOperation* operation);
  void SendProcessLoop(const std::stop_token& stop_token);
  void ReceiveProcessLoop(const std::stop_token& stop_token);
  void HandleReceivedMessage(zmq::multipart_t multi);
  void SendGossip();
  void SendFirstGossipMessage(const NodeInfo& contact_node);
  void HandleGossip(zmq::message_t gossip_msg);

  struct ReplyOperation {
    Identifier identifier;
    ByteBufferType data;
  };

  class DeferredOperations {
   public:
    void Add(const uint32_t& node_id, ReplyOperation operation) {
      std::lock_guard lock {mutex_};
      map_[node_id].push_back(std::move(operation));
    }

    std::vector<ReplyOperation> TryMoveOut(const uint32_t& node_id) {
      std::lock_guard lock {mutex_};
      auto it = map_.find(node_id);
      if (it == map_.end()) return {};
      auto operations = std::move(it->second);
      map_.erase(it);
      return operations;
    }

   private:
    std::unordered_map<uint32_t, std::vector<ReplyOperation>> map_;
    std::mutex mutex_;
  };

  NodeInfo this_node_ {};
  std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler_;
  NetworkConfig network_config_;
  std::atomic_uint64_t send_request_id_counter_ = 0;
  std::atomic_uint64_t received_request_id_counter_ = 0;

  zmq::context_t context_ {/*io_threads_=*/1};
  LockGuardedMap<uint32_t, zmq::socket_t> node_id_to_send_socket_;
  DeferredOperations node_id_to_pending_replies_;
  zmq::socket_t recv_socket_ {context_, zmq::socket_type::dealer};

  LinearizableUnboundedMpscQueue<TypeErasedSendOperation*> pending_send_operations_;
  LockGuardedMap<uint64_t, TypeErasedSendOperation*> send_request_id_to_operation_;
  LinearizableUnboundedMpscQueue<ReplyOperation> pending_reply_operations_;
  LockGuardedMap<uint64_t, Identifier> received_request_id_to_identifier_;

  std::jthread send_thread_;
  std::jthread recv_thread_;
  std::atomic_bool stopped_ = false;
  NodeInfoManager node_manager_;
  exec::async_scope async_scope_;

  uint64_t last_heartbeat_ms_;
};

}  // namespace ex_actor::internal
