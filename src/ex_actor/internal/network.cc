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
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/serialization.h"
#include "stdexec/__detail/__sync_wait.hpp"

namespace ex_actor::internal::network {

MessageBroker::MessageBroker(std::vector<NodeInfo> node_list, uint32_t this_node_id,
                             std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler,
                             NetworkConfig network_config)
    : this_node_({.node_id = this_node_id}),
      request_handler_(std::move(request_handler)),
      network_config_(network_config),
      last_heartbeat_(std::chrono::steady_clock::now()) {
  FindContactNode(node_list);
  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

MessageBroker::MessageBroker(const ClusterConfig& cluster_config,
                             std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler)
    : this_node_(cluster_config.this_node),
      request_handler_(std::move(request_handler)),
      network_config_(cluster_config.network_config),
      last_heartbeat_(std::chrono::steady_clock::now()) {
  EXA_THROW_CHECK(!this_node_.address.empty())
      << "Local address not found in node list, this_node_id: " << this_node_.node_id;
  recv_socket_.bind(this_node_.address);
  recv_socket_.set(zmq::sockopt::linger, 0);
  logging::Info("Node {}'s recv socket bound to {}", this_node_.node_id, this_node_.address);

  if (!cluster_config.contact_node.address.empty()) {
    EstablishConnectionTo(cluster_config.contact_node);
    SendNodeInfoToContactNode(cluster_config.contact_node);
  }

  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

MessageBroker::~MessageBroker() {
  if (!stopped_.load()) {
    ClusterAlignedStop();
  }
}

void MessageBroker::ClusterAlignedStop() {
  // tell all other nodes: I'm going to quit
  logging::Info("[Cluster Aligned Stop] Node {} sending quit message to all other nodes", this_node_.node_id);
  const auto node_list = peer_nodes_.GetNodeList();

  for (const auto& node : node_list) {
    if (node.node_id != this_node_.node_id) {
      auto sender = SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kQuit) | ex::then([](auto empty) {});
      async_scope_.spawn(std::move(sender));
    }
  }

  peer_nodes_.WaitAllNodesExit();
  stopped_.store(true);
  ex::sync_wait(async_scope_.on_empty());
  logging::Info("[Cluster Aligned Stop] All nodes are going to quit, stopping node {}'s io threads.",
                this_node_.node_id);
  // stop io threads first
  send_thread_.request_stop();
  recv_thread_.request_stop();
  send_thread_.join();
  recv_thread_.join();
  logging::Info("[Cluster Aligned Stop] Node {}'s io threads stopped, cluster aligned stop completed",
                this_node_.node_id);
}

void MessageBroker::EstablishConnectionTo(const NodeInfo& node_info) {
  const auto& node_id = node_info.node_id;
  const auto& node_address = node_info.address;
  bool inserted = node_id_to_send_socket_.Insert(node_id, zmq::socket_t(context_, zmq::socket_type::dealer));
  EXA_THROW_CHECK(inserted) << "Node " << node_id << " already has a send socket";
  auto& send_socket = node_id_to_send_socket_.At(node_id);
  send_socket.set(zmq::sockopt::linger, 0);
  send_socket.connect(node_address);
  logging::Info("Node {} added a send socket, connected to node {} at {}", this_node_.node_id, node_id, node_address);

  peer_nodes_.Add(node_info, {.liveness = PeerNodes::Liveness::kAlive, .last_seen = GetTimeMs()});
}

void MessageBroker::FindContactNode(const std::vector<NodeInfo>& node_list) {
  // Bind router socket to this node's address
  NodeInfo contact_node {.node_id = UINT32_MAX};

  for (const auto& node : node_list) {
    if (node.node_id == this_node_.node_id) {
      this_node_.address = node.address;
      recv_socket_.bind(node.address);
      // Setting linger to 0 instructs the socket to discard any unsent messages immediately and return control to the
      // without waiting when it is closed.
      recv_socket_.set(zmq::sockopt::linger, 0);
      logging::Info("Node {}'s recv socket bound to {}", this_node_.node_id, node.address);
    }
    if (node.node_id < contact_node.node_id) {
      contact_node = node;
    }
  }
  if (this_node_.node_id == contact_node.node_id) {
    return;
  }

  EXA_THROW_CHECK(this_node_.address.size() > 0)
      << "Local address not found in node list, this_node_id: " << this_node_.node_id;
  EstablishConnectionTo(contact_node);
  SendNodeInfoToContactNode(contact_node);
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
  util::SetThreadName("snd_proc_loop");
  while (!stop_token.stop_requested()) {
    bool any_item_pulled = false;
    while (auto optional_operation = pending_send_operations_.TryPop()) {
      auto* operation = optional_operation.value();
      auto serialized_identifier = internal::serde::Serialize(operation->identifier);
      zmq::multipart_t multi;
      multi.addmem(serialized_identifier.data(), serialized_identifier.size());
      multi.add(std::move(operation->data));
      auto& send_socket = node_id_to_send_socket_.At(operation->identifier.response_node_id);
      last_heartbeat_ = std::chrono::steady_clock::now();
      EXA_THROW_CHECK(multi.send(send_socket));
      if (operation->identifier.flag == MessageFlag::kQuit || operation->identifier.flag == MessageFlag::kGossip) {
        // quit operation,heartbeat and gossip has no response, complete it immediately
        operation->Complete(ByteBufferType {});
      }
      any_item_pulled = true;
    }
    while (auto optional_reply_operation = pending_reply_operations_.TryPop()) {
      auto& reply_operation = optional_reply_operation.value();
      auto& send_socket = node_id_to_send_socket_.At(reply_operation.identifier.request_node_id);
      auto serialized_identifier = internal::serde::Serialize(reply_operation.identifier);
      zmq::multipart_t multi;
      multi.addmem(serialized_identifier.data(), serialized_identifier.size());
      multi.add(std::move(reply_operation.data));
      last_heartbeat_ = std::chrono::steady_clock::now();
      EXA_THROW_CHECK(multi.send(send_socket));
      any_item_pulled = true;
    }
    if (!any_item_pulled) {
      SendGossip();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void MessageBroker::ReceiveProcessLoop(const std::stop_token& stop_token) {
  util::SetThreadName("recv_proc_loop");
  recv_socket_.set(zmq::sockopt::rcvtimeo, 100);

  while (!stop_token.stop_requested()) {
    CheckHeartbeat();
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

  auto identifier = internal::serde::Deserialize<Identifier>(identifier_bytes.data<uint8_t>(), identifier_bytes.size());

  // all received messages will update the last seen time;
  if (identifier.flag == MessageFlag::kGossip) {
    // NOTE: We will update the last_seen of peer nodes in the HandleGossip
    HandleGossip(std::move(data_bytes));
    return;
  }

  // For responses, the peer is response_node_id.
  uint32_t peer_node_id =
      (identifier.request_node_id == this_node_.node_id) ? identifier.response_node_id : identifier.request_node_id;
  peer_nodes_.RefreshLastSeen(peer_node_id, GetTimeMs());

  if (identifier.flag == MessageFlag::kQuit) {
    EXA_THROW_CHECK_EQ(data_bytes.size(), 0) << "Quit message should not have data";
    logging::Info("[Cluster Aligned Stop] Node {} is going to quit", identifier.request_node_id);
    peer_nodes_.DeactivateNode(identifier.request_node_id);
    return;
  }

  // Update the node list

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

void MessageBroker::CheckHeartbeat() { peer_nodes_.CheckHeartbeat(network_config_.heartbeat_timeout); }

// TODO: The gossip logic need the send/recv thread to  serialize/deserialize node_list, which seems not a
// good idea.

void MessageBroker::SendGossip() {
  if (!stopped_.load() && std::chrono::steady_clock::now() - last_heartbeat_ >= network_config_.gossip_interval) {
    // TODO: The size of the peers should be configurable
    const auto node_list = peer_nodes_.GetRandomPeers(2);
    auto message = peer_nodes_.GenerateGossipMessage();
    message.emplace_back(this_node_, GetTimeMs());
    auto serialized_message = serde::Serialize(serde::GossipMessage {message});
    serde::BufferWriter writer {ByteBufferType(serialized_message.size())};
    writer.CopyFrom(serialized_message.data(), serialized_message.size());
    auto payload = std::move(writer).MoveBufferOut();
    for (const auto& node : node_list) {
      ByteBufferType per_node_payload;
      per_node_payload.copy(payload);
      auto gossip =
          SendRequest(node.node_id, std::move(per_node_payload), MessageFlag::kGossip) | ex::then([](auto&& null) {});
      last_heartbeat_ = std::chrono::steady_clock::now();
      logging::Info("[Gossip] node {} send gossip to  node {}", this_node_.node_id, node.node_id);
      async_scope_.spawn(std::move(gossip));
    }
  }
}

void MessageBroker::SendNodeInfoToContactNode(const NodeInfo& contact_node) {
  std::vector<GossipMessage> message {{.node_info = this_node_, .last_seen = GetTimeMs()}};
  auto serialized_message = serde::Serialize(serde::GossipMessage {message});
  serde::BufferWriter writer {ByteBufferType(serialized_message.size())};
  writer.CopyFrom(serialized_message.data(), serialized_message.size());
  auto gossip = SendRequest(contact_node.node_id, std::move(writer).MoveBufferOut(), MessageFlag::kGossip) |
                ex::then([](auto&& null) {});
  last_heartbeat_ = std::chrono::steady_clock::now();
  logging::Info("[Gossip] node {} send gossip to  node {}", this_node_.node_id, contact_node.node_id);
  async_scope_.spawn(std::move(gossip));
}

bool MessageBroker::CheckNode(uint32_t node_id) { return peer_nodes_.Contains(node_id); }

void MessageBroker::HandleGossip(zmq::message_t gossip_msg) {
  const auto messages =
      serde::Deserialize<serde::GossipMessage>(gossip_msg.data<uint8_t>(), gossip_msg.size()).messages;
  for (const auto& msg : messages) {
    const auto& node = msg.node_info;
    const auto& last_seen = msg.last_seen;
    if (node.node_id == this_node_.node_id) {
      continue;
    }
    if (!peer_nodes_.Contains(node.node_id)) {
      EstablishConnectionTo(node);
    }
    peer_nodes_.RefreshLastSeen(node.node_id, last_seen);
  }
}

}  // namespace ex_actor::internal::network
