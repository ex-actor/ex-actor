#include "ex_actor/internal/network.h"

#include <chrono>
#include <thread>

#include <spdlog/spdlog.h>
#include <zmq_addon.hpp>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal::network {
MessageBroker::MessageBroker(std::vector<ex_actor::NodeInfo> node_list, uint32_t this_node_id)
    : node_list_(std::move(node_list)), this_node_id_(this_node_id) {
  logging::SetupProcessWideLoggingConfig();
  EstablishConnections();
}

void MessageBroker::StartIOThreads() {
  send_thread_ = std::jthread([this](const std::stop_token& stop_token) { SendProcessLoop(stop_token); });
  recv_thread_ = std::jthread([this](const std::stop_token& stop_token) { ReceiveProcessLoop(stop_token); });
}

void MessageBroker::EstablishConnections() {
  // Bind router socket to this node's address
  bool found_local_address = false;
  for (const auto& node : node_list_) {
    if (node.node_id == this_node_id_) {
      recv_socket_.bind(node.address);
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
      send_socket.connect(node.address);
      spdlog::info("Node {} added a send socket, connected to node {} at {}", this_node_id_, node.node_id,
                   node.address);
    }
  }
}

MessageBroker::SendRequestSender MessageBroker::SendRequest(uint32_t to_node_id, ByteBufferType data) {
  EXA_THROW_CHECK_NE(to_node_id, this_node_id_) << "Cannot send message to current node";
  Identifier identifier {
      .request_node_id = this_node_id_,
      .response_node_id = to_node_id,
      .request_id_in_node = send_request_id_counter_.fetch_add(1),
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

void MessageBroker::PushOperation(TypeErasedOperation* operation) {
  {
    std::scoped_lock lock(map_mutex_);
    send_request_id_to_operation_.Insert(operation->identifier.request_id_in_node, operation);
  }
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
      EXA_THROW_CHECK(multi.send(send_socket));
    }
    while (auto optional_reply_operation = pending_reply_operations_.TryPop()) {
      auto& reply_operation = optional_reply_operation.value();
      auto& send_socket = node_id_to_send_socket_.At(reply_operation.identifier.request_node_id);
      auto serialized_identifier = internal::serde::Serialize(reply_operation.identifier);
      zmq::multipart_t multi;
      multi.addmem(serialized_identifier.data(), serialized_identifier.size());
      multi.add(std::move(reply_operation.data));
      EXA_THROW_CHECK(multi.send(send_socket));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MessageBroker::ReceiveProcessLoop(const std::stop_token& stop_token) {
  util::SetThreadName("recv_proc_loop");
  recv_socket_.set(zmq::sockopt::rcvtimeo, 100);

  while (!stop_token.stop_requested()) {
    zmq::multipart_t multi;
    if (!multi.recv(recv_socket_)) {
      continue;
    }

    // Validate message structure: [identifier][data]
    EXA_THROW_CHECK_EQ(multi.size(), 2) << "Expected 2-part message, got " << multi.size() << " parts";

    // Extract frames
    zmq::message_t identifier_bytes = multi.pop();
    zmq::message_t data_bytes = multi.pop();

    auto identifier =
        internal::serde::Deserialize<Identifier>(identifier_bytes.data<uint8_t>(), identifier_bytes.size());

    if (identifier.request_node_id == this_node_id_) {
      // Response from remote node
      TypeErasedOperation* operation = send_request_id_to_operation_.At(identifier.request_id_in_node);
      send_request_id_to_operation_.Erase(identifier.request_id_in_node);
      operation->Complete(std::move(data_bytes));
    } else if (identifier.response_node_id == this_node_id_) {
      // Request from remote node - pass to handler, which will send response back
      auto receive_request_id = received_request_id_counter_.fetch_add(1);
      received_request_id_to_identifier_.Insert(receive_request_id, identifier);
      HandleRequestFromOtherNode(receive_request_id, std::move(data_bytes));
    } else {
      EXA_THROW << "Invalid identifier, " << EXA_DUMP_VARS(identifier);
    }
  }
}
}  // namespace ex_actor::internal::network