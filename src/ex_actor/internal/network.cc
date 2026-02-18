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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {

NodeInfoManager::NodeInfoManager(uint32_t this_node_id) : this_node_id_(this_node_id) {}

void NodeInfoManager::Add(uint32_t node_id, const NodeState& state) {
  std::lock_guard lock(mutex_);
  auto [iter, inserted] = node_id_to_state_.try_emplace(node_id, state);
  if (!inserted) {
    iter->second.address = state.address;
    iter->second.liveness = Liveness::kAlive;
    iter->second.last_seen = GetTimeMs();
  }
  alive_peers_ += 1;
}

void NodeInfoManager::RefreshLastSeen(uint32_t node_id, uint64_t last_seen) {
  std::lock_guard lock(mutex_);
  auto [iter, inserted] = node_id_to_state_.try_emplace(
      node_id, NodeState {.liveness = Liveness::kConnecting, .last_seen = last_seen, .address = {}});
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
    return liveness != Liveness::kDead && liveness != Liveness::kConnecting;
  }
  return false;
}

bool NodeInfoManager::Contains(uint32_t node_id) {
  std::lock_guard lock(mutex_);
  return node_id_to_state_.contains(node_id);
}

void NodeInfoManager::DeactivateNode(uint32_t node_id) {
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

void NodeInfoManager::WaitAllNodesExit() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this]() { return alive_peers_ == 0; });
}

std::vector<NodeInfo> NodeInfoManager::GetHealthyNodeList() {
  std::vector<NodeInfo> node_list {};
  node_list.reserve(node_id_to_state_.size());
  std::lock_guard lock(mutex_);
  for (const auto& pair : node_id_to_state_) {
    const auto& liveness = pair.second.liveness;
    if (liveness != Liveness::kDead && liveness != Liveness::kConnecting) {
      node_list.emplace_back(pair.first, pair.second.address);
    }
  }
  return node_list;
}

void NodeInfoManager::PrintAllNodesState(uint32_t this_node_id) {
  std::vector<NodeInfo> node_list {};
  node_list.reserve(node_id_to_state_.size());
  std::lock_guard lock(mutex_);
  log::Info("[PeerNodes] This node {}", this_node_id);
  for (const auto& pair : node_id_to_state_) {
    log::Info("[PeerNodes] Node {}, address {}, state {} ", pair.first, pair.second.address,
              static_cast<uint8_t>(pair.second.liveness));
  }
}

std::vector<GossipMessage> NodeInfoManager::GenerateGossipMessage() {
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

std::vector<NodeInfo> NodeInfoManager::GetRandomPeers(size_t size) {
  std::vector<NodeInfo> nodes;

  {
    std::lock_guard lock(mutex_);
    if (node_id_to_state_.empty()) {
      return nodes;
    }
    nodes.reserve(node_id_to_state_.size());
    for (const auto& pair : node_id_to_state_) {
      // We should not send gossip messages to quitting nodes.
      if (pair.second.liveness == Liveness::kAlive) {
        nodes.emplace_back(pair.first, pair.second.address);
      }
    }
  }

  // We only call this method in the send_thread, so we don't need to hold lock here.
  std::shuffle(nodes.begin(), nodes.end(), rng_);
  nodes.resize(std::min(nodes.size(), size));
  return nodes;
}

exec::task<bool> NodeInfoManager::WaitNodeAlive(uint32_t node_id, std::chrono::milliseconds timeout) {
  std::shared_ptr<Waiter> waiter;
  {
    std::lock_guard lock(mutex_);
    if (auto iter = node_id_to_state_.find(node_id); iter != node_id_to_state_.end()) {
      auto liveness = iter->second.liveness;
      if (liveness != Liveness::kDead && liveness != Liveness::kConnecting) {
        co_return true;
      }
    }
    waiter = std::make_shared<Waiter>(std::chrono::steady_clock::now() + timeout);
    node_id_to_waiters_[node_id].push_back(waiter);
  }

  co_await waiter->sem.OnDrained();
  co_return waiter->arrive.load(std::memory_order_relaxed);
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
    waiter->arrive.store(true, std::memory_order_relaxed);
    waiter->sem.Acquire(1);
  }
}

void NodeInfoManager::NotifyAllWaiters() {
  std::vector<std::shared_ptr<Waiter>> waiters {};
  {
    std::lock_guard lock(mutex_);
    waiters.reserve(node_id_to_waiters_.size() * 2);
    for (auto& pair : node_id_to_waiters_) {
      for (auto& waiter : pair.second) {
        waiters.push_back(std::move(waiter));
      }
    }
    node_id_to_waiters_.clear();
  }

  for (auto& waiter : waiters) {
    waiter->arrive.store(true, std::memory_order_relaxed);
    waiter->sem.Acquire(1);
  }
}

void NodeInfoManager::CheckHeartbeatAndExpireWaiters(std::chrono::milliseconds timeout) {
  std::vector<std::shared_ptr<Waiter>> expired;

  {
    std::lock_guard lock(mutex_);
    for (const auto& pair : node_id_to_state_) {
      const auto& state = pair.second;
      if (state.liveness == Liveness::kAlive &&
          GetTimeMs() - state.last_seen >= std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()) {
        log::Error("Node {} detects that node {} is dead, try to exit", this_node_id_, pair.first);
        // don't call static variables' destructors, or the program will hang in MessageBroker's destructor
        std::quick_exit(1);
      }
    }
    for (auto& pair : node_id_to_waiters_) {
      auto& vec = pair.second;
      std::erase_if(vec, [&](auto& waiter) {
        if (waiter->deadline <= std::chrono::steady_clock::now()) {
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
}

MessageBroker::MessageBroker(const ClusterConfig& cluster_config,
                             std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler)
    : this_node_(cluster_config.this_node),
      request_handler_(std::move(request_handler)),
      node_mngr_(cluster_config.this_node.node_id),
      network_config_(cluster_config.network_config),
      last_heartbeat_(std::chrono::steady_clock::now()) {
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
  if (!stopped_.load(std::memory_order_relaxed)) {
    ClusterAlignedStop();
  }
}

void MessageBroker::ClusterAlignedStop() {
  // tell all other nodes: I'm going to quit
  log::Info("[Cluster Aligned Stop] Node {} sending quit message to all other nodes", this_node_.node_id);
  const auto node_list = node_mngr_.GetHealthyNodeList();

  for (const auto& node : node_list) {
    if (node.node_id != this_node_.node_id) {
      auto sender = SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kQuit) | ex::then([](auto empty) {});
      async_scope_.spawn(std::move(sender));
    }
  }

  // This will stop gossip
  stopped_.store(true, std::memory_order_release);
  node_mngr_.WaitAllNodesExit();
  ex::sync_wait(async_scope_.on_empty());
  log::Info("[Cluster Aligned Stop] All nodes are going to quit, stopping node {}'s io threads.", this_node_.node_id);
  // stop io threads first
  send_thread_.request_stop();
  recv_thread_.request_stop();
  send_thread_.join();
  recv_thread_.join();
  log::Info("[Cluster Aligned Stop] Node {}'s io threads stopped, cluster aligned stop completed", this_node_.node_id);
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

  node_mngr_.Add(
      node_info.node_id,
      {.liveness = NodeInfoManager::Liveness::kAlive, .last_seen = GetTimeMs(), .address = node_info.address});
  auto pending_repies = node_id_to_pending_replies_.TryMoveOut(node_id);
  auto pending_requests = node_id_to_pending_request_.TryMoveOut(node_id);
  for (auto&& reply : pending_repies) {
    pending_reply_operations_.Push(std::move(reply));
  }

  for (auto* request : pending_requests) {
    pending_send_operations_.Push(request);
  }

  node_mngr_.NotifyWaiters(node_id);
}

void MessageBroker::EstablishConnection(const std::vector<NodeInfo>& node_list) {
  // Bind router socket to this node's address
  NodeInfo contact_node {.node_id = UINT32_MAX};

  for (const auto& node : node_list) {
    if (node.node_id == this_node_.node_id) {
      this_node_.address = node.address;
      recv_socket_.bind(node.address);
      // Setting linger to 0 instructs the socket to discard any unsent messages immediately and return control to the
      // without waiting when it is closed.
      recv_socket_.set(zmq::sockopt::linger, 0);
      log::Info("Node {}'s recv socket bound to {}", this_node_.node_id, node.address);
    }
    if (node.node_id < contact_node.node_id) {
      contact_node = node;
    }
  }
  if (this_node_.node_id == contact_node.node_id) {
    return;
  }

  EXA_THROW_CHECK(!this_node_.address.empty())
      << "Local address not found in node list, this_node_id: " << this_node_.node_id;
  EstablishConnectionTo(contact_node);
}

MessageBroker::SendRequestSender MessageBroker::SendRequest(uint32_t to_node_id, ByteBufferType data,
                                                            MessageFlag flag) {
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

void MessageBroker::ReplyRequest(uint64_t received_request_id, ByteBufferType data) {
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
        multi.add(std::move(operation->data));
        auto& send_socket = node_id_to_send_socket_.At(operation->identifier.response_node_id);
        EXA_THROW_CHECK(multi.send(send_socket));
        if (operation->identifier.flag == MessageFlag::kQuit || operation->identifier.flag == MessageFlag::kGossip) {
          operation->Complete(ByteBufferType {});
        }
        any_item_pulled = true;
      } else {
        node_id_to_pending_request_.Add(response_node_id, operation);
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
        multi.add(std::move(reply_operation.data));
        EXA_THROW_CHECK(multi.send(send_socket));
        any_item_pulled = true;
      } else {
        node_id_to_pending_replies_.Add(request_node_id, std::move(reply_operation));
      }
    }
    if (!any_item_pulled) {
      SendGossip();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void MessageBroker::ReceiveProcessLoop(const std::stop_token& stop_token) {
  SetThreadName("recv_proc_loop");
  recv_socket_.set(zmq::sockopt::rcvtimeo, 100);

  while (!stop_token.stop_requested()) {
    node_mngr_.CheckHeartbeatAndExpireWaiters(network_config_.heartbeat_timeout);
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

  auto identifier = Deserialize<Identifier>(identifier_bytes.data<uint8_t>(), identifier_bytes.size());

  // all received messages will update the last seen time;
  // For responses, the peer is response_node_id.
  uint32_t peer_node_id =
      (identifier.request_node_id == this_node_.node_id) ? identifier.response_node_id : identifier.request_node_id;
  node_mngr_.RefreshLastSeen(peer_node_id, GetTimeMs());

  if (identifier.flag == MessageFlag::kQuit) {
    EXA_THROW_CHECK_EQ(data_bytes.size(), 0) << "Quit message should not have data";
    log::Info("[Cluster Aligned Stop] Node {} detects node {} is going to quit", this_node_.node_id,
              identifier.request_node_id);
    node_mngr_.DeactivateNode(identifier.request_node_id);
    return;
  }

  if (identifier.flag == MessageFlag::kGossip) {
    HandleGossip(std::move(data_bytes));
    return;
  }

  if (identifier.request_node_id == this_node_.node_id) {
    // Response from remote node
    TypeErasedSendOperation* operation = send_request_id_to_operation_.At(identifier.request_id_in_node);
    send_request_id_to_operation_.Erase(identifier.request_id_in_node);
    operation->Complete(std::move(data_bytes));
  } else if (identifier.response_node_id == this_node_.node_id) {
    // Request from remote node - pass to handler, which will send response back
    auto received_request_id = received_request_id_counter_.fetch_add(1);
    received_request_id_to_identifier_.Insert(received_request_id, identifier);
    request_handler_(received_request_id, std::move(data_bytes));
  } else {
    EXA_THROW << "Invalid identifier, " << EXA_DUMP_VARS(identifier);
  }
}

void MessageBroker::SendGossip() {
  if (!stopped_.load(std::memory_order_relaxed) &&
      std::chrono::steady_clock::now() - last_heartbeat_ >= network_config_.gossip_interval) {
    const auto node_list = node_mngr_.GetRandomPeers(std::max(1U, network_config_.gossip_fanout));
    auto message = node_mngr_.GenerateGossipMessage();
    message.emplace_back(this_node_, GetTimeMs());
    auto serialized_message = Serialize(GossipPayload {message});
    BufferWriter writer {ByteBufferType(serialized_message.size())};
    writer.CopyFrom(serialized_message.data(), serialized_message.size());
    auto payload = std::move(writer).MoveBufferOut();

    for (const auto& node : node_list) {
      ByteBufferType per_node_payload;
      per_node_payload.copy(payload);
      auto gossip =
          SendRequest(node.node_id, std::move(per_node_payload), MessageFlag::kGossip) | ex::then([](auto&& null) {});
      async_scope_.spawn(std::move(gossip));
    }
    last_heartbeat_ = std::chrono::steady_clock::now();
  }
}

void MessageBroker::HandleGossip(zmq::message_t gossip_msg) {
  const auto messages = Deserialize<GossipPayload>(gossip_msg.data<uint8_t>(), gossip_msg.size()).messages;
  for (const auto& msg : messages) {
    const auto& node = msg.node_info;
    const auto& last_seen = msg.last_seen;
    if (node.node_id == this_node_.node_id) {
      if (node.address != this_node_.address) {
        EXA_THROW << "Nodes with the same node ID but different addresses exist in the cluster.";
      }
      continue;
    }

    // We shouldn't add new peer node for a quitting node.
    if (!node_mngr_.Connected(node.node_id, node.address) && !stopped_.load(std::memory_order_relaxed)) {
      EstablishConnectionTo(node);
    }
    node_mngr_.RefreshLastSeen(node.node_id, last_seen);
  }
}

bool MessageBroker::CheckNodeConnected(const uint32_t node_id) { return node_mngr_.Connected(node_id); }

exec::task<bool> MessageBroker::WaitNodeAlive(uint32_t node_id, std::chrono::milliseconds timeout) {
  co_return co_await node_mngr_.WaitNodeAlive(node_id, timeout);
}

}  // namespace ex_actor::internal
