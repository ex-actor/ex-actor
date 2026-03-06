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

#include "ex_actor/internal/network.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

namespace {
inline uint64_t GetTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::span<const std::byte> ZmqMsgBytes(const zmq::message_t& msg) {
  return {static_cast<const std::byte*>(msg.data()), msg.size()};
}

ex_actor::internal::ByteBuffer ZmqMsgToBytes(zmq::message_t&& msg) {
  auto span = ZmqMsgBytes(msg);
  // TODO: a copy here, optimize it in the future
  return {span.begin(), span.end()};
}

zmq::message_t BytesToZmqMsg(ex_actor::internal::ByteBuffer&& bytes) {
  if (bytes.empty()) return {};
  auto* owned = new ex_actor::internal::ByteBuffer(std::move(bytes));
  auto deleter = [](void*, void* hint) { delete static_cast<ex_actor::internal::ByteBuffer*>(hint); };
  return zmq::message_t(owned->data(), owned->size(), deleter, owned);
}
}  // namespace

namespace ex_actor::internal {

NodeInfoManager::NodeInfoManager(uint32_t this_node_id) : this_node_id_(this_node_id) {}

void NodeInfoManager::Add(uint32_t node_id, const NodeState& state) {
  std::lock_guard lock(mutex_);
  auto [iter, inserted] = node_id_to_state_.try_emplace(node_id, state);

  if (inserted || iter->second.liveness == NodeState::Liveness::kConnecting) {
    if (!inserted) {
      // RefreshLastSeen() may insert a kConnecting entry with an empty address.
      // Update it here once we actually establish a connection.
      iter->second = state;  // avoid extra lookup
    }
    alive_peers_ += 1;
    return;
  }

  EXA_THROW << fmt_lib::format("Attempted to add an already-connected node to NodeInfoManager, node_id={}", node_id);
}

void NodeInfoManager::RefreshLastSeen(uint32_t node_id, uint64_t last_seen) {
  std::lock_guard lock(mutex_);
  // Every received message triggers this, but we might hear from a node we never connected to
  // (e.g., a node about to quit whose info was gossiped). Insert a placeholder with an empty
  // address to avoid errors.
  auto [iter, inserted] = node_id_to_state_.try_emplace(
      node_id, NodeState {.liveness = NodeState::Liveness::kConnecting, .last_seen = last_seen});
  if (!inserted) {
    iter->second.last_seen = std::max(iter->second.last_seen, last_seen);
  }
}

bool NodeInfoManager::Connected(uint32_t node_id, const std::string& address) {
  std::lock_guard lock(mutex_);
  if (auto iter = node_id_to_state_.find(node_id); iter != node_id_to_state_.end()) {
    if (!address.empty() && !iter->second.address.empty() && address != iter->second.address) {
      EXA_THROW << "Nodes with the same node ID but different addresses exist in the cluster.";
    }
    auto liveness = iter->second.liveness;
    return liveness != NodeState::Liveness::kDead && liveness != NodeState::Liveness::kConnecting;
  }
  return false;
}

bool NodeInfoManager::Contains(uint32_t node_id) {
  std::lock_guard lock(mutex_);
  return node_id_to_state_.contains(node_id);
}

std::vector<NodeInfo> NodeInfoManager::GetHealthyNodeList() {
  std::vector<NodeInfo> node_list {};
  std::lock_guard lock(mutex_);
  node_list.reserve(node_id_to_state_.size());
  for (const auto& pair : node_id_to_state_) {
    const auto& liveness = pair.second.liveness;
    if (liveness != NodeState::Liveness::kDead && liveness != NodeState::Liveness::kConnecting) {
      node_list.emplace_back(pair.first, pair.second.address);
    }
  }
  return node_list;
}

GossipMessage NodeInfoManager::GenerateGossipMessage() {
  GossipMessage message;
  std::lock_guard lock(mutex_);
  message.node_states.reserve(node_id_to_state_.size());
  for (const auto& pair : node_id_to_state_) {
    if (pair.second.liveness == NodeState::Liveness::kAlive) {
      message.node_states.push_back({.liveness = NodeState::Liveness::kAlive,
                                     .last_seen = pair.second.last_seen,
                                     .node_id = pair.first,
                                     .address = pair.second.address});
    }
  }
  return message;
}

std::vector<NodeInfo> NodeInfoManager::GetRandomPeers(size_t size) {
  std::vector<NodeInfo> nodes;
  {
    std::lock_guard lock(mutex_);
    if (node_id_to_state_.empty()) {
      return nodes;
    }
    size = std::clamp(size, size_t {1}, node_id_to_state_.size());
    nodes.reserve(node_id_to_state_.size());
    for (const auto& pair : node_id_to_state_) {
      if (pair.second.liveness == NodeState::Liveness::kAlive) {
        nodes.emplace_back(pair.first, pair.second.address);
      }
    }
  }

  // We only call this method in the send_thread, so we don't need to hold lock here.
  std::shuffle(nodes.begin(), nodes.end(), rng_);
  nodes.resize(std::min(nodes.size(), size));
  return nodes;
}

exec::task<bool> NodeInfoManager::WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  std::shared_ptr<Waiter> waiter;
  {
    std::lock_guard lock(mutex_);
    if (auto iter = node_id_to_state_.find(node_id); iter != node_id_to_state_.end()) {
      auto liveness = iter->second.liveness;
      if (liveness != NodeState::Liveness::kDead && liveness != NodeState::Liveness::kConnecting) {
        co_return true;
      }
    }
    waiter = std::make_shared<Waiter>(GetTimeMs() + timeout_ms);
    node_id_to_waiters_[node_id].push_back(waiter);
  }

  co_await waiter->sem.OnDrained();
  co_return waiter->arrive.load(std::memory_order_acquire);
}

void NodeInfoManager::NotifyWaiters(uint32_t node_id) {
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
    waiter->arrive.store(true, std::memory_order_release);
    waiter->sem.Acquire(1);
  }
}

void NodeInfoManager::DeactivateNode(uint32_t node_id) {
  auto it = node_id_to_state_.find(node_id);
  if (it == node_id_to_state_.end() || it->second.liveness != NodeState::Liveness::kAlive) {
    return;
  }
  it->second.liveness = NodeState::Liveness::kDead;
  alive_peers_ -= 1;
}

std::vector<uint32_t> NodeInfoManager::CheckHeartbeatAndExpireWaiters(uint64_t timeout_ms) {
  std::vector<std::shared_ptr<Waiter>> expired;
  std::vector<uint32_t> newly_dead;

  {
    std::lock_guard lock(mutex_);
    for (auto& pair : node_id_to_state_) {
      auto& state = pair.second;
      if (state.liveness == NodeState::Liveness::kAlive && GetTimeMs() - state.last_seen >= timeout_ms) {
        log::Warn(
            "Node {} detects that node {} is dead(no heartbeat in last {}ms), all requests to this node will be failed",
            this_node_id_, pair.first, timeout_ms);
        DeactivateNode(pair.first);
        newly_dead.push_back(pair.first);
      }
    }
    auto now_ms = GetTimeMs();
    for (auto& pair : node_id_to_waiters_) {
      auto& vec = pair.second;
      std::erase_if(vec, [&expired, now_ms](auto& waiter) {
        if (waiter->deadline_ms <= now_ms) {
          expired.push_back(std::move(waiter));
          return true;
        }
        return false;
      });
    }
  }

  for (auto& waiter : expired) {
    waiter->sem.Acquire(1);
  }
  return newly_dead;
}

MessageBroker::MessageBroker(const ClusterConfig& cluster_config,
                             std::function<void(uint64_t received_request_id, ByteBuffer data)> request_handler)
    : this_node_(cluster_config.this_node),
      request_handler_(std::move(request_handler)),
      node_manager_(cluster_config.this_node.node_id),
      network_config_(cluster_config.network_config),
      last_heartbeat_ms_(GetTimeMs()) {
  if (!this_node_.address.empty()) {
    recv_socket_.bind(this_node_.address);
    recv_socket_.set(zmq::sockopt::linger, 0);
    log::Info("Node {}'s recv socket bound to {}", this_node_.node_id, this_node_.address);

    if (!cluster_config.contact_node.address.empty()) {
      EXA_THROW_CHECK(this_node_.address != cluster_config.contact_node.address &&
                      this_node_.node_id != cluster_config.contact_node.node_id)
          << "The local node has the same node ID or address as the contact node.";
      EstablishConnectionTo(cluster_config.contact_node);
    }
  }

  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

MessageBroker::~MessageBroker() {
  if (!stopped_.load(std::memory_order_acquire)) {
    Stop();
  }
}

void MessageBroker::Stop() {
  log::Info("Node {} stopping message broker", this_node_.node_id);
  stopped_.store(true, std::memory_order_release);
  ex::sync_wait(async_scope_.on_empty());
  send_thread_.request_stop();
  recv_thread_.request_stop();
  send_thread_.join();
  recv_thread_.join();
  log::Info("Node {}'s message broker stopped", this_node_.node_id);
}

void MessageBroker::EstablishConnectionTo(const NodeInfo& node_info) {
  const auto& node_id = node_info.node_id;
  const auto& node_address = node_info.address;
  bool inserted = node_id_to_send_socket_.Insert(node_id, zmq::socket_t(context_, zmq::socket_type::dealer));
  EXA_THROW_CHECK(inserted) << "Node " << node_id << " already has a send socket";
  auto& send_socket = node_id_to_send_socket_.At(node_id);
  send_socket.set(zmq::sockopt::linger, 0);
  send_socket.connect(node_address);
  log::Info("[Gossip] Node {} found node {}, connected to it at {}", this_node_.node_id, node_id, node_address);

  node_manager_.Add(node_info.node_id, {.liveness = NodeState::Liveness::kAlive,
                                        .last_seen = GetTimeMs(),
                                        .node_id = node_info.node_id,
                                        .address = node_info.address});
  auto pending_replies = node_id_to_pending_replies_.TryMoveOut(node_id);
  for (auto&& reply : pending_replies) {
    pending_reply_operations_.Push(std::move(reply));
  }

  node_manager_.NotifyWaiters(node_id);
}

MessageBroker::SendRequestSender MessageBroker::SendRequest(uint32_t to_node_id, ByteBuffer data, MessageFlag flag) {
  EXA_THROW_CHECK_NE(to_node_id, this_node_.node_id) << "Cannot send message to current node";
  Identifier identifier {
      .request_node_id = this_node_.node_id,
      .response_node_id = to_node_id,
      .request_id_in_node = send_request_id_counter_.fetch_add(1),
      .flag = flag,
  };
  return SendRequestSender {
      .identifier = identifier,
      .data = std::move(data),
      .message_broker = this,
  };
}

void MessageBroker::ReplyRequest(uint64_t received_request_id, ByteBuffer data) {
  auto identifier = received_request_id_to_identifier_.At(received_request_id);
  received_request_id_to_identifier_.Erase(received_request_id);

  pending_reply_operations_.Push(ReplyOperation {
      .identifier = identifier,
      .data = std::move(data),
  });
}

void MessageBroker::PushOperation(TypeErasedSendOperation* operation) {
  send_request_id_to_operation_.Insert(operation->identifier.request_id_in_node, operation);
  pending_send_operations_.Push(operation);
}

void MessageBroker::SendProcessLoop(const std::stop_token& stop_token) {
  SetThreadName("snd_proc_loop");
  while (!stop_token.stop_requested()) {
    bool any_item_pulled = false;
    while (auto optional_operation = pending_send_operations_.TryPop()) {
      auto* operation = optional_operation.value();
      auto response_node_id = operation->identifier.response_node_id;
      if (node_id_to_send_socket_.Contains(response_node_id)) {
        auto serialized_identifier = Serialize(operation->identifier);
        zmq::multipart_t multi;
        multi.addmem(serialized_identifier.data(), serialized_identifier.size());
        multi.add(BytesToZmqMsg(std::move(operation->data)));
        auto& send_socket = node_id_to_send_socket_.At(operation->identifier.response_node_id);
        EXA_THROW_CHECK(multi.send(send_socket));
        if (operation->identifier.flag == MessageFlag::kGossip) {
          send_request_id_to_operation_.Erase(operation->identifier.request_id_in_node);
          operation->Complete(ByteBuffer {});
        }
        any_item_pulled = true;
      } else {
        send_request_id_to_operation_.Erase(operation->identifier.request_id_in_node);
        operation->SetError(std::make_exception_ptr(
            ex_actor::internal::ThrowStream() << fmt_lib::format(
                "Node {} is trying to send request to an unconnected node {}", this_node_.node_id, response_node_id)));
      }
    }

    while (auto optional_reply_operation = pending_reply_operations_.TryPop()) {
      auto& reply_operation = optional_reply_operation.value();
      auto request_node_id = reply_operation.identifier.request_node_id;
      if (node_id_to_send_socket_.Contains(request_node_id)) {
        auto& send_socket = node_id_to_send_socket_.At(request_node_id);
        auto serialized_identifier = Serialize(reply_operation.identifier);
        zmq::multipart_t multi;
        multi.addmem(serialized_identifier.data(), serialized_identifier.size());
        multi.add(BytesToZmqMsg(std::move(reply_operation.data)));
        EXA_THROW_CHECK(multi.send(send_socket));
        any_item_pulled = true;
      } else {
        node_id_to_pending_replies_.Add(request_node_id, std::move(reply_operation));
        // If the response_node arrive before Add, this new added reply_operation won't be pushed into
        // pending_reply_operations_ and will get stuck;
        if (node_id_to_send_socket_.Contains(request_node_id)) {
          std::ranges::for_each(node_id_to_pending_replies_.TryMoveOut(request_node_id),
                                [this](ReplyOperation& reply) { pending_reply_operations_.Push(std::move(reply)); });
        }
      }
    }

    SendGossip();

    if (!any_item_pulled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void MessageBroker::ReceiveProcessLoop(const std::stop_token& stop_token) {
  SetThreadName("recv_proc_loop");
  recv_socket_.set(zmq::sockopt::rcvtimeo, 100);

  while (!stop_token.stop_requested()) {
    auto dead_nodes = node_manager_.CheckHeartbeatAndExpireWaiters(network_config_.heartbeat_timeout_ms);
    for (auto dead_node_id : dead_nodes) {
      ErrorOutPendingOperations(dead_node_id);
    }
    zmq::multipart_t multi;
    if (!multi.recv(recv_socket_)) {
      continue;
    }
    HandleReceivedMessage(std::move(multi));
  }
}

void MessageBroker::HandleReceivedMessage(zmq::multipart_t multi) {
  // Validate message structure: [identifier][data]
  EXA_THROW_CHECK_EQ(multi.size(), 2) << "Expected 2-part message, got " << multi.size() << " parts";

  // Extract frames
  zmq::message_t identifier_bytes = multi.pop();
  zmq::message_t data_bytes = multi.pop();

  auto identifier = Deserialize<Identifier>(ZmqMsgBytes(identifier_bytes));

  // all received messages will update the last seen time;
  // For responses, the peer is response_node_id.
  uint32_t peer_node_id =
      (identifier.request_node_id == this_node_.node_id) ? identifier.response_node_id : identifier.request_node_id;
  node_manager_.RefreshLastSeen(peer_node_id, GetTimeMs());

  if (identifier.flag == MessageFlag::kGossip) {
    HandleGossip(std::move(data_bytes));
    return;
  }

  if (identifier.request_node_id == this_node_.node_id) {
    // Response from remote node
    TypeErasedSendOperation* operation = send_request_id_to_operation_.At(identifier.request_id_in_node);
    send_request_id_to_operation_.Erase(identifier.request_id_in_node);
    operation->Complete(ZmqMsgToBytes(std::move(data_bytes)));
  } else if (identifier.response_node_id == this_node_.node_id) {
    // Request from remote node - pass to handler, which will send response back
    auto received_request_id = received_request_id_counter_.fetch_add(1);
    received_request_id_to_identifier_.Insert(received_request_id, identifier);
    request_handler_(received_request_id, ZmqMsgToBytes(std::move(data_bytes)));
  } else {
    EXA_THROW << "Invalid identifier, " << EXA_DUMP_VARS(identifier);
  }
}

void MessageBroker::SendGossip() {
  if (!stopped_.load(std::memory_order_acquire) &&
      GetTimeMs() - last_heartbeat_ms_ >= network_config_.gossip_interval_ms) {
    const auto node_list = node_manager_.GetRandomPeers(network_config_.gossip_fanout);
    auto message = node_manager_.GenerateGossipMessage();
    message.node_states.push_back({.liveness = NodeState::Liveness::kAlive,
                                   .last_seen = GetTimeMs(),
                                   .node_id = this_node_.node_id,
                                   .address = this_node_.address});
    auto payload = Serialize(message);

    for (const auto& node : node_list) {
      auto gossip = SendRequest(node.node_id, ByteBuffer(payload), MessageFlag::kGossip) | ex::then([](auto&& null) {});
      async_scope_.spawn(std::move(gossip));
    }
    last_heartbeat_ms_ = GetTimeMs();
  }
}

void MessageBroker::HandleGossip(zmq::message_t gossip_msg) {
  const auto message = Deserialize<GossipMessage>(ZmqMsgBytes(gossip_msg));
  for (const auto& state : message.node_states) {
    if (state.node_id == this_node_.node_id) {
      if (state.address != this_node_.address) {
        EXA_THROW << "Nodes with the same node ID but different addresses exist in the cluster.";
      }
      continue;
    }

    if (!node_manager_.Connected(state.node_id, state.address) && !stopped_.load(std::memory_order_acquire)) {
      EstablishConnectionTo({.node_id = state.node_id, .address = state.address});
    }
    node_manager_.RefreshLastSeen(state.node_id, state.last_seen);
  }
}

void MessageBroker::ErrorOutPendingOperations(uint32_t dead_node_id) {
  std::vector<TypeErasedSendOperation*> to_error;
  {
    std::lock_guard lock(send_request_id_to_operation_.GetMutex());
    auto& map = send_request_id_to_operation_.GetMap();
    for (auto it = map.begin(); it != map.end();) {
      if (it->second->identifier.response_node_id == dead_node_id) {
        to_error.push_back(it->second);
        it = map.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto* op : to_error) {
    op->SetError(std::make_exception_ptr(ThrowStream()
                                         << fmt_lib::format("Node {} is dead, cannot complete request", dead_node_id)));
  }

  // Clean up received requests from the dead node that we haven't replied to yet.
  // The dead node will never read the reply, so keeping these leaks memory.
  {
    std::lock_guard lock(received_request_id_to_identifier_.GetMutex());
    auto& map = received_request_id_to_identifier_.GetMap();
    for (auto it = map.begin(); it != map.end();) {
      if (it->second.request_node_id == dead_node_id) {
        it = map.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Discard deferred replies queued for the dead node.
  node_id_to_pending_replies_.TryMoveOut(dead_node_id);
}

bool MessageBroker::CheckNodeConnected(const uint32_t node_id) { return node_manager_.Connected(node_id); }

exec::task<bool> MessageBroker::WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  co_return co_await node_manager_.WaitNodeAlive(node_id, timeout_ms);
}

}  // namespace ex_actor::internal
