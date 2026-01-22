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
#include "ex_actor/internal/util.h"

namespace ex_actor {
struct NodeInfo {
  uint32_t node_id = 0;
  std::string address;
  friend bool operator==(const NodeInfo& lhs, const NodeInfo& rhs) { return lhs.node_id == rhs.node_id; }
};
}  // namespace ex_actor

namespace std {
// The current implementation can not reuse the same node_id for different address
template <>
struct hash<ex_actor::NodeInfo> {
  size_t operator()(const ex_actor::NodeInfo& k) const noexcept { return hash<uint32_t> {}(k.node_id); }
};
}  // namespace std

namespace ex_actor::internal::network {
using ByteBufferType = zmq::message_t;
using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;

inline uint64_t GetTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

enum class MessageFlag : uint8_t { kNormal = 0, kQuit, kGossip };

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
  uint64_t last_seen;
};

struct Waiter {
  explicit Waiter(TimePoint deadline) : sem(1), deadline(deadline) {}
  ex_actor::util::Semaphore sem;
  TimePoint deadline;
};

class PeerNodes {
 public:
  enum class Liveness : uint8_t { kAlive = 0, kConnecting, kQuitting, kDead };
  struct NodeState {
    Liveness liveness;
    uint64_t last_seen;
    std::string address;
  };
  void Add(const uint32_t node_id, const NodeState& state) {
    std::lock_guard lock(mutex_);
    auto [iter, inserted] = node_id_to_state_.try_emplace(node_id, state);
    if (!inserted) {
      iter->second.address = state.address;
      iter->second.liveness = Liveness::kAlive;
      iter->second.last_seen = GetTimeMs();
    }
    alive_peers_ += 1;
  }

  void RefreshLastSeen(const uint32_t node_id, const uint64_t last_seen) {
    std::lock_guard lock(mutex_);
    auto [iter, inserted] = node_id_to_state_.try_emplace(
        node_id, NodeState {.liveness = Liveness::kConnecting, .last_seen = last_seen, .address = {}});
    if (!inserted) {
      iter->second.last_seen = std::max(iter->second.last_seen, last_seen);
    }
  }

  void CheckNodeHeartbeat(const std::chrono::milliseconds& timeout) {
    std::lock_guard lock(mutex_);
    for (const auto& pair : node_id_to_state_) {
      const auto& state = pair.second;
      if (state.liveness == Liveness::kAlive &&
          GetTimeMs() - state.last_seen >= std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()) {
        logging::Error("Node {} is dead, try to exit", pair.first);
        // don't call static variables' destructors, or the program will hang in MessageBroker's destructor
        std::quick_exit(1);
      }
    }
  }

  bool Connected(const uint32_t node_id) {
    std::lock_guard lock(mutex_);
    if (auto iter = node_id_to_state_.find(node_id); iter != node_id_to_state_.end()) {
      auto liveness = iter->second.liveness;
      return liveness != Liveness::kDead && liveness != Liveness::kConnecting;
    }
    return false;
  }

  bool Contains(const uint32_t node_id) {
    std::lock_guard lock(mutex_);
    return node_id_to_state_.contains(node_id);
  }

  void DeactivateNode(const uint32_t node_id) {
    std::lock_guard lock(mutex_);
    auto& state = node_id_to_state_.at(node_id);
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

  std::vector<NodeInfo> GetHealthyNodeList() {
    std::vector<NodeInfo> node_list {};
    node_list.reserve(node_id_to_state_.size());
    std::lock_guard lock(mutex_);
    for (const auto& pair : node_id_to_state_) {
      const auto& node = pair.first;
      const auto& liveness = pair.second.liveness;
      if (liveness != Liveness::kDead && liveness != Liveness::kConnecting) {
        node_list.emplace_back(pair.first, pair.second.address);
      }
    }
    return node_list;
  }

  void PrintAllNodesState(const uint32_t this_node_id) {
    std::vector<NodeInfo> node_list {};
    node_list.reserve(node_id_to_state_.size());
    std::lock_guard lock(mutex_);
    logging::Info("[PeerNodes] This node {} is going to exit", this_node_id);
    for (const auto& pair : node_id_to_state_) {
      logging::Info("[PeerNodes] Node {}, address {}, state {} ", pair.first, pair.second.address,
                    static_cast<uint8_t>(pair.second.liveness));
    }
  }

  std::vector<GossipMessage> GenerateGossipMessage() {
    std::vector<GossipMessage> messages;
    messages.reserve(node_id_to_state_.size());
    std::lock_guard lock(mutex_);
    for (const auto& pair : node_id_to_state_) {
      if (pair.second.liveness == Liveness::kAlive) {
        messages.push_back(
            {.node_info = {.node_id = pair.first, .address = pair.second.address}, .last_seen = pair.second.last_seen});
      }
    }
    return messages;
  }

  std::vector<NodeInfo> GetRandomPeers(const size_t size) {
    std::vector<NodeInfo> nodes;
    if (size == 0) {
      return nodes;
    }

    {
      std::lock_guard lock(mutex_);
      nodes.reserve(node_id_to_state_.size());
      size_t seen = 0;
      for (const auto& pair : node_id_to_state_) {
        // NOTE: We should not send gossip messages to quitting nodes.
        if (pair.second.liveness == Liveness::kAlive) {
          nodes.emplace_back(pair.first, pair.second.address);
        }
      }
    }

    // NOTE: We only call this method in the send_thread, so we don't need to hold lock here.
    std::shuffle(nodes.begin(), nodes.end(), rng_);
    nodes.resize(std::min(nodes.size(), size));
    return nodes;
  }

  std::shared_ptr<Waiter> TryRegisterWaiter(uint32_t node_id, std::chrono::milliseconds timeout) {
    std::lock_guard lock(mutex_);
    if (auto iter = node_id_to_state_.find(node_id); iter != node_id_to_state_.end()) {
      auto liveness = iter->second.liveness;
      if (liveness != Liveness::kDead && liveness != Liveness::kConnecting) {
        return nullptr;
      }
    }
    auto waiter = std::make_shared<Waiter>(std::chrono::steady_clock::now() + timeout);
    node_id_to_waiters_[node_id].push_back(std::move(waiter));

    return waiter;
  }

  void NotifyWaiters(uint32_t node_id) {
    std::vector<std::shared_ptr<Waiter>> waiters;

    {
      std::lock_guard lock(mutex_);
      auto it = node_id_to_waiters_.find(node_id);
      if (it == node_id_to_waiters_.end()) {
        return;
      }
      waiters = std::move(it->second);
      node_id_to_waiters_.erase(it);
    }

    for (auto& waiter : waiters) {
      waiter->sem.Acquire(1);
    }
  }

  void ExpireWaiters() {
    std::vector<std::shared_ptr<Waiter>> expired;

    {
      std::lock_guard lock(mutex_);
      auto now = std::chrono::steady_clock::now();
      for (auto& pair : node_id_to_waiters_) {
        auto& vec = pair.second;
        std::erase_if(vec, [&](auto& waiter) {
          if (waiter->deadline <= std::chrono::steady_clock::now()) {
            expired.push_back(waiter);
            return true;
          }
          return false;
        });
      }
    }

    for (auto& waiter : expired) {
      waiter->sem.Acquire(1);
    }
  }

 private:
  std::unordered_map<uint32_t, NodeState> node_id_to_state_;
  std::unordered_map<uint32_t, std::vector<std::shared_ptr<Waiter>>> node_id_to_waiters_;
  std::condition_variable cv_;
  std::mutex mutex_;
  std::mt19937 rng_ {std::random_device {}()};
  uint32_t alive_peers_ = 0;
};

class MessageBroker {
 public:
  explicit MessageBroker(const std::vector<NodeInfo>& node_list, uint32_t this_node_id,
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

  bool CheckNodeConnected(uint32_t node_id);

  exec::task<bool> WaitNodeAlive(uint32_t node_id, std::chrono::milliseconds timeout);

 private:
  void EstablishConnectionTo(const NodeInfo& node_info);
  void EstablishConnection(const std::vector<NodeInfo>& node_list);
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

  template <typename Operation>
  class DeferredOperations {
   public:
    void Add(const uint32_t& node_id, Operation operation) {
      std::lock_guard lock {mutex_};
      map_[node_id].push_back(std::move(operation));
    }

    std::vector<Operation> TryMoveOut(const uint32_t& node_id) {
      std::lock_guard lock {mutex_};
      auto it = map_.find(node_id);
      if (it == map_.end()) return {};
      auto operations = std::move(it->second);
      map_.erase(it);
      return operations;
    }

   private:
    std::unordered_map<uint32_t, std::vector<Operation>> map_;
    std::mutex mutex_;
  };

  NodeInfo this_node_ {};
  std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler_;
  NetworkConfig network_config_;
  std::atomic_uint64_t send_request_id_counter_ = 0;
  std::atomic_uint64_t received_request_id_counter_ = 0;
  size_t contact_node_index_ = 0;

  zmq::context_t context_ {/*io_threads_=*/1};
  util::LockGuardedMap<uint32_t, zmq::socket_t> node_id_to_send_socket_;
  DeferredOperations<ReplyOperation> node_id_to_pending_replies_;
  DeferredOperations<TypeErasedSendOperation*> node_id_to_pending_request_;
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

  TimePoint last_heartbeat_;
};

}  // namespace ex_actor::internal::network

namespace ex_actor {
using internal::network::ClusterConfig;
using internal::network::NetworkConfig;
}  // namespace ex_actor
