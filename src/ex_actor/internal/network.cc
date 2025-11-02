#include "ex_actor/internal/network.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <utility>

#include <exec/async_scope.hpp>
#include <spdlog/spdlog.h>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal::network {

MessageBroker::MessageBroker(std::vector<ex_actor::NodeInfo> node_list, uint32_t this_node_id,
                             std::function<void(uint64_t receive_request_id, ByteBufferType data)> request_handler,
                             HeartbeatConfig hearbeat_config)
    : node_list_(std::move(node_list)),
      this_node_id_(this_node_id),
      request_handler_(std::move(request_handler)),
      hearbeat_(hearbeat_config),
      quit_latch_(node_list_.size()),
      last_heartbeat_(std::chrono::steady_clock::now()) {
  logging::SetupProcessWideLoggingConfig();
  EstablishConnections();

  auto start_time_point = std::chrono::steady_clock::now();
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      last_seen_.emplace(node.node_id, start_time_point);
    }
  }

  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

MessageBroker::~MessageBroker() {
  if (!stopped_) {
    ClusterAlignedStop();
  }
}

void MessageBroker::ClusterAlignedStop() {
  // tell all other nodes: I'm going to quit
  spdlog::info("[Cluster Aligned Stop] Node {} sending quit message to all other nodes", this_node_id_);
  for (const auto& node : node_list_) {
    if (node.node_id != this_node_id_) {
      auto sender = SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kQuit);
      // TODO: async_scope.spawn() won't compile, figure it out later
      stdexec::sync_wait(std::move(sender));
    }
  }

  quit_latch_.count_down();
  // wait until all nodes are going to quit
  quit_latch_.wait();
  stopped_ = true;
  spdlog::info("[Cluster Aligned Stop] All nodes are going to quit, stopping node {}'s io threads.", this_node_id_);
  // stop io threads first
  send_thread_.request_stop();
  recv_thread_.request_stop();
  send_thread_.join();
  recv_thread_.join();
  spdlog::info("[Cluster Aligned Stop] Node {}'s io threads stopped, cluster aligned stop completed", this_node_id_);
}

void MessageBroker::EstablishConnections() {
  // Bind router socket to this node's address
  bool found_local_address = false;
  for (const auto& node : node_list_) {
    if (node.node_id == this_node_id_) {
      recv_socket_.bind(node.address);
      recv_socket_.set(zmq::sockopt::linger, 0);
      found_local_address = true;
      spdlog::info("Node {}'s recv socket bound to {}", this_node_id_, node.address);
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
      spdlog::info("Node {} added a send socket, connected to node {} at {}", this_node_id_, node.node_id,
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

void MessageBroker::ReplyRequest(uint64_t receive_request_id, ByteBufferType data) {
  auto identifier = received_request_id_to_identifier_.At(receive_request_id);
  received_request_id_to_identifier_.Erase(receive_request_id);

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
    while (auto optional_operation = pending_send_operations_.TryPop()) {
      auto* operation = optional_operation.value();
      auto serialized_identifier = internal::serde::Serialize(operation->identifier);
      zmq::multipart_t multi;
      multi.addmem(serialized_identifier.data(), serialized_identifier.size());
      multi.add(std::move(operation->data));
      auto& send_socket = node_id_to_send_socket_.At(operation->identifier.response_node_id);
      last_heartbeat_ = std::chrono::steady_clock::now();
      EXA_THROW_CHECK(multi.send(send_socket));
      if (operation->identifier.flag == MessageFlag::kQuit || operation->identifier.flag == MessageFlag::kHeartbeat) {
        // quit operation and heartbeat has no response, complete it immediately
        operation->Complete(ByteBufferType {});
      }
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
    }
    SendHeartbeat();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
  if (identifier.flag == MessageFlag::kQuit) {
    EXA_THROW_CHECK_EQ(data_bytes.size(), 0) << "Quit message should not have data";
    spdlog::info("[Cluster Aligned Stop] Node {} is going to quit", identifier.request_node_id);
    quit_latch_.count_down();
    return;
  }

  // Request, response and heartbeat will update the last seen time;
  last_seen_[identifier.request_node_id] = std::chrono::steady_clock::now();
  if (identifier.flag == MessageFlag::kHeartbeat) {
    return;
  }

  if (identifier.request_node_id == this_node_id_) {
    // Response from remote node
    TypeErasedSendOperation* operation = send_request_id_to_operation_.At(identifier.request_id_in_node);
    send_request_id_to_operation_.Erase(identifier.request_id_in_node);
    operation->Complete(std::move(data_bytes));
  } else if (identifier.response_node_id == this_node_id_) {
    // Request from remote node - pass to handler, which will send response back
    auto receive_request_id = received_request_id_counter_.fetch_add(1);
    received_request_id_to_identifier_.Insert(receive_request_id, identifier);
    request_handler_(receive_request_id, std::move(data_bytes));
  } else {
    EXA_THROW << "Invalid identifier, " << EXA_DUMP_VARS(identifier);
  }
}

void MessageBroker::CheckHeartbeat() {
  for (auto& node : node_list_) {
    if (node.node_id != this_node_id_ &&
        std::chrono::steady_clock::now() - last_seen_[node.node_id] >= hearbeat_.heartbeat_timeout) {
      spdlog::error("Node {} detect that node {} is dead, try to exit", this_node_id_, node.node_id);
      std::exit(1);
    }
  }
}

void MessageBroker::SendHeartbeat() {
  if (std::chrono::steady_clock::now() - last_heartbeat_ >= hearbeat_.heartbeat_interval) {
    for (auto& node : node_list_) {
      if (node.node_id != this_node_id_) {
        // Use ex::then to suppress a compilation error, which is likely caused by
        // SendRequestSender not supporting cancellation.
        auto hearbeat =
            SendRequest(node.node_id, ByteBufferType {}, MessageFlag::kHeartbeat) | ex::then([](auto&& null) {});
        async_scope_.spawn(std::move(hearbeat));
      }
    }
  }
}

}  // namespace ex_actor::internal::network
