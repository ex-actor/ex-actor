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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>
#include <utility>

#include <exec/async_scope.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal::network {

MessageBroker::MessageBroker(std::vector<NodeInfo> node_list, uint32_t this_node_id,
                             std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler,
                             NetworkConfig network_config)
    : node_list_(std::move(node_list)),
      this_node_id_(this_node_id),
      request_handler_(std::move(request_handler)),
      network_config_(network_config),
      last_heartbeat_(std::chrono::steady_clock::now()),
      last_gossip_(std::chrono::steady_clock::now()) {
  EstablishConnections();

  auto start_time_point = std::chrono::steady_clock::now();
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      last_seen_.emplace(node.node_id, start_time_point);
      peer_nodes_.Insert(node, true);
    }
  }
  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

MessageBroker::MessageBroker(const ClusterConfig& cluster_config,
                             std::function<void(uint64_t received_request_id, ByteBufferType data)> request_handler)
    : this_node_id_(cluster_config.this_node.node_id),
      request_handler_(std::move(request_handler)),
      network_config_(cluster_config.network_config),
      last_heartbeat_(std::chrono::steady_clock::now()),
      last_gossip_(std::chrono::steady_clock::now()),
      enable_dynamic_connectivity_(true) {
  auto this_node = cluster_config.this_node;
  EXA_THROW_CHECK(!this_node.address.empty())
      << "Local address not found in node list, this_node_id: " << this_node_id_;

  recv_socket_.bind(this_node.address);
  recv_socket_.set(zmq::sockopt::linger, 0);
  logging::Info("Node {}'s recv socket bound to {}", this_node_id_, this_node.address);

  EstablishConnectionTo(cluster_config.contact_node);

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
  logging::Info("[Cluster Aligned Stop] Node {} sending quit message to all other nodes", this_node_id_);
  if (enable_dynamic_connectivity_) {
    node_list_ = peer_nodes_.GetNodeList();
  }

  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      auto sender = SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kQuit) | ex::then([](auto empty) {});
      async_scope_.spawn(std::move(sender));
    }
  }

  peer_nodes_.WaitAllNodesExit();
  stopped_.store(true);
  ex::sync_wait(async_scope_.on_empty());
  logging::Info("[Cluster Aligned Stop] All nodes are going to quit, stopping node {}'s io threads.", this_node_id_);
  // stop io threads first
  send_thread_.request_stop();
  recv_thread_.request_stop();
  send_thread_.join();
  recv_thread_.join();
  logging::Info("[Cluster Aligned Stop] Node {}'s io threads stopped, cluster aligned stop completed", this_node_id_);
}

void MessageBroker::EstablishConnectionTo(const NodeInfo& node_info) {
  const auto& node_id = node_info.node_id;
  const auto& node_address = node_info.address;
  bool inserted = node_id_to_send_socket_.Insert(node_id, zmq::socket_t(context_, zmq::socket_type::dealer));
  EXA_THROW_CHECK(inserted) << "Node " << node_id << " already has a send socket";
  auto& send_socket = node_id_to_send_socket_.At(node_id);
  send_socket.set(zmq::sockopt::linger, 0);
  send_socket.connect(node_address);
  logging::Info("Node {} added a send socket, connected to node {} at {}", this_node_id_, node_id, node_address);

  last_seen_.emplace(node_id, std::chrono::steady_clock::now());
  peer_nodes_.Insert(node_info, true);
}

void MessageBroker::EstablishConnections() {
  // Bind router socket to this node's address
  bool found_local_address = false;
  for (const auto& node : node_list_) {
    if (node.node_id == this_node_id_) {
      recv_socket_.bind(node.address);
      // Setting linger to 0 instructs the socket to discard any unsent messages immediately and return control to the
      // without waiting when it is closed.
      recv_socket_.set(zmq::sockopt::linger, 0);
      found_local_address = true;
      logging::Info("Node {}'s recv socket bound to {}", this_node_id_, node.address);
      break;
    }
  }

  EXA_THROW_CHECK(found_local_address) << "Local address not found in node list, this_node_id: " << this_node_id_;

  // Connect router socket to all other nodes (mesh topology)
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      bool inserted = node_id_to_send_socket_.Insert(node.node_id, zmq::socket_t(context_, zmq::socket_type::dealer));
      EXA_THROW_CHECK(inserted) << "Node " << node.node_id << " already has a send socket";
      auto& send_socket = node_id_to_send_socket_.At(node.node_id);
      send_socket.set(zmq::sockopt::linger, 0);
      send_socket.connect(node.address);
      logging::Info("Node {} added a send socket, connected to node {} at {}", this_node_id_, node.node_id,
                    node.address);
    }
  }
}

MessageBroker::SendRequestSender MessageBroker::SendRequest(uint32_t to_node_id, ByteBufferType data,
                                                            MessageFlag flag) {
  EXA_THROW_CHECK_NE(to_node_id, this_node_id_) << "Cannot send message to current node";
  Identifier identifier {
      .request_node_id = this_node_id_,
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
      if (operation->identifier.flag == MessageFlag::kQuit || operation->identifier.flag == MessageFlag::kHeartbeat ||
          operation->identifier.flag == MessageFlag::kGossip) {
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
    if (enable_dynamic_connectivity_) {
      SendGossip();
    }
    SendHeartbeat();
    if (!any_item_pulled) {
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
  last_seen_[identifier.request_node_id] = std::chrono::steady_clock::now();

  if (identifier.flag == MessageFlag::kQuit) {
    EXA_THROW_CHECK_EQ(data_bytes.size(), 0) << "Quit message should not have data";
    logging::Info("[Cluster Aligned Stop] Node {} is going to quit", identifier.request_node_id);
    peer_nodes_.DeactivateNode(identifier.request_node_id);
    return;
  }

  if (identifier.flag == MessageFlag::kHeartbeat) {
    return;
  }

  // Update the node list
  if (identifier.flag == MessageFlag::kGossip) {
    HandleGossip(std::move(data_bytes));
    return;
  }

  if (identifier.request_node_id == this_node_id_) {
    // Response from remote node
    TypeErasedSendOperation* operation = send_request_id_to_operation_.At(identifier.request_id_in_node);
    send_request_id_to_operation_.Erase(identifier.request_id_in_node);
    operation->Complete(std::move(data_bytes));
  } else if (identifier.response_node_id == this_node_id_) {
    // Request from remote node - pass to handler, which will send response back
    auto received_request_id = received_request_id_counter_.fetch_add(1);
    received_request_id_to_identifier_.Insert(received_request_id, identifier);
    request_handler_(received_request_id, std::move(data_bytes));
  } else {
    EXA_THROW << "Invalid identifier, " << EXA_DUMP_VARS(identifier);
  }
}

void MessageBroker::CheckHeartbeat() {
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_ &&
        std::chrono::steady_clock::now() - last_seen_[node.node_id] >= network_config_.heartbeat_timeout) {
      logging::Error("Node {} detect that node {} is dead, try to exit", this_node_id_, node.node_id);
      // don't call static variables' destructors, or the program will hang in MessageBroker's destructor
      std::quick_exit(1);
    }
  }
}

void MessageBroker::SendHeartbeat() {
  if (!stopped_.load() && std::chrono::steady_clock::now() - last_heartbeat_ >= network_config_.heartbeat_interval) {
    for (const auto& node : node_list_) {
      if (node.node_id != this_node_id_) {
        auto heartbeat =
            SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kHeartbeat) | ex::then([](auto&& null) {});
        last_heartbeat_ = std::chrono::steady_clock::now();
        async_scope_.spawn(std::move(heartbeat));
      }
    }
  }
}

bool MessageBroker::CheckNode(uint32_t node_id) {
  return std::ranges::find_if(node_list_, [node_id](auto& element) { return element.node_id == node_id; }) !=
         node_list_.end();
}

// TODO: Funky
size_t MessageBroker::NextContactNode() {
  contact_node_index_ += 1;
  contact_node_index_ %= node_list_.size();
  return contact_node_index_;
}

// TODO: The gossip logic need the send/recv thread to  serialize/deserialize node_list, which seems not a
// good idea.

void MessageBroker::SendGossip() {
  if (node_list_.size() == 1) {
    return;
  }

  if (!stopped_.load() && std::chrono::steady_clock::now() - last_gossip_ >= network_config_.gossip_interval) {
    auto serialized_node_list = serde::Serialize(serde::GossipNodeList {node_list_});
    serde::BufferWriter writer {network::ByteBufferType(serialized_node_list.size())};
    writer.CopyFrom(serialized_node_list.data(), serialized_node_list.size());
    // TODO: Funky
    auto contact_node = node_list_.at(NextContactNode());
    if (contact_node.node_id == this_node_id_) {
      contact_node = node_list_.at(NextContactNode());
    }
    auto gossip = SendRequest(contact_node.node_id, std::move(writer).MoveBufferOut(), MessageFlag::kGossip) |
                  ex::then([](auto&& null) {});
    last_gossip_ = std::chrono::steady_clock::now();
    logging::Info("[Gossip] node {} send gossip to  node {}", this_node_id_, contact_node.node_id);
    async_scope_.spawn(std::move(gossip));
  }
}

void MessageBroker::HandleGossip(zmq::message_t gossip_msg) {
  auto nodes = serde::Deserialize<serde::GossipNodeList>(gossip_msg.data<uint8_t>(), gossip_msg.size()).node_list;
  for (auto& node : nodes) {
    if (node.node_id == this_node_id_) {
      continue;
    }

    if (!peer_nodes_.Contains(node)) {
      peer_nodes_.Insert(node, true);
      // We only read/modify last_seen_ in the recv thread, so it's safe here.
      last_seen_.try_emplace(node.node_id, std::chrono::steady_clock::now());
      bool inserted = node_id_to_send_socket_.Insert(node.node_id, zmq::socket_t(context_, zmq::socket_type::dealer));
      if (inserted) {
        auto& send_socket = node_id_to_send_socket_.At(node.node_id);
        send_socket.set(zmq::sockopt::linger, 0);
        send_socket.connect(node.address);
        logging::Info("[Gossip] Node {} have found node {}, connected it at {}", this_node_id_, node.node_id,
                      node.address);
      }
    }
  }
}
}  // namespace ex_actor::internal::network
