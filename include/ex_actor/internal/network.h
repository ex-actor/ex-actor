#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <exec/task.hpp>
#include <zmq.hpp>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {
struct NodeInfo {
  uint32_t node_id = 0;
  std::string address;
};
}  // namespace ex_actor

namespace ex_actor::internal::network {
using ByteBufferType = zmq::message_t;

struct Identifier {
  uint32_t request_node_id;
  uint32_t response_node_id;
  uint64_t request_id_in_node;
};

class MessageBroker {
 public:
  explicit MessageBroker(std::vector<NodeInfo> node_list, uint32_t this_node_id);
  virtual ~MessageBroker() = default;

  void StartIOThreads();

  // -------- std::execution sender adaption start--------
  struct TypeErasedOperation {
    enum class Type : uint8_t {
      kRequest = 0,
      kReply = 1,
    };
    virtual ~TypeErasedOperation() = default;
    virtual void Complete(ByteBufferType response_data) = 0;
    TypeErasedOperation(Type type, Identifier identifier, ByteBufferType data, MessageBroker* message_broker)
        : type(type), identifier(identifier), data(std::move(data)), message_broker(message_broker) {}
    Type type;
    Identifier identifier;
    ByteBufferType data;
    MessageBroker* message_broker {};
  };
  template <ex::receiver R>
  struct SendRequestOperation : TypeErasedOperation {
    SendRequestOperation(Identifier identifier, ByteBufferType data, MessageBroker* message_broker, R receiver)
        : TypeErasedOperation(Type::kRequest, identifier, std::move(data), message_broker),
          receiver(std::move(receiver)) {}
    R receiver;
    std::atomic_bool started = false;
    void start() noexcept {
      bool expected = false;
      bool changed = started.compare_exchange_strong(expected, true);
      if (!changed) [[unlikely]] {
        spdlog::critical("MessageBroker Operation already started");
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
  SendRequestSender SendRequest(uint32_t to_node_id, ByteBufferType data);

  void ReplyRequest(uint64_t receive_request_id, ByteBufferType data);

 protected:
  virtual void HandleRequestFromOtherNode(uint64_t receive_request_id, ByteBufferType data) = 0;

 private:
  void EstablishConnections();
  void PushOperation(TypeErasedOperation* operation);
  void SendProcessLoop(const std::stop_token& stop_token);
  void ReceiveProcessLoop(const std::stop_token& stop_token);

  struct ReplyOperation {
    Identifier identifier;
    ByteBufferType data;
  };

  std::vector<NodeInfo> node_list_;
  uint32_t this_node_id_;
  std::function<void(uint64_t receive_request_id, ByteBufferType data)> request_handler_;
  std::atomic_uint64_t send_request_id_counter_ = 0;
  std::atomic_uint64_t received_request_id_counter_ = 0;

  zmq::context_t context_ {/*io_threads_=*/1};
  util::LockGuardedMap<uint32_t, zmq::socket_t> node_id_to_send_socket_;
  zmq::socket_t recv_socket_ {context_, zmq::socket_type::dealer};

  std::jthread send_thread_;
  std::jthread recv_thread_;
  util::LinearizableUnboundedQueue<TypeErasedOperation*> pending_send_operations_;
  util::LockGuardedMap<uint64_t, TypeErasedOperation*> send_request_id_to_operation_;
  util::LinearizableUnboundedQueue<ReplyOperation> pending_reply_operations_;
  util::LockGuardedMap<uint64_t, Identifier> received_request_id_to_identifier_;
  std::mutex map_mutex_;
};

}  // namespace ex_actor::internal::network