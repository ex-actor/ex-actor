#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <latch>
#include <mutex>
#include <thread>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "ex_actor/internal/util.h"

namespace ex_actor {
struct NodeInfo {
  uint32_t node_id = 0;
  std::string address;
};
}  // namespace ex_actor

namespace ex_actor::internal::network {
using ByteBufferType = zmq::message_t;

enum class MessageFlag : uint8_t { kNormal = 0, kQuit, kHeartbeat };

struct Identifier {
  uint32_t request_node_id;
  uint32_t response_node_id;
  uint64_t request_id_in_node;
  MessageFlag flag;
};

class MessageBroker {
 public:
  explicit MessageBroker(std::vector<NodeInfo> node_list, uint32_t this_node_id,
                         std::function<void(uint64_t receive_request_id, ByteBufferType data)> request_handler);
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
  SendRequestSender SendRequest(uint32_t to_node_id, ByteBufferType data, MessageFlag flag = MessageFlag::kNormal);

  void ReplyRequest(uint64_t receive_request_id, ByteBufferType data);

 private:
  void EstablishConnections();
  void PushOperation(TypeErasedSendOperation* operation);
  void SendProcessLoop(const std::stop_token& stop_token);
  void ReceiveProcessLoop(const std::stop_token& stop_token);
  void HandleReceivedMessage(zmq::multipart_t multi);
  void CheckHeartbeat(std::chrono::milliseconds waitting_time);
  void SendHeartbeat(std::chrono::milliseconds period);

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

  util::LinearizableUnboundedQueue<TypeErasedSendOperation*> pending_send_operations_;
  util::LockGuardedMap<uint64_t, TypeErasedSendOperation*> send_request_id_to_operation_;
  util::LinearizableUnboundedQueue<ReplyOperation> pending_reply_operations_;
  util::LockGuardedMap<uint64_t, Identifier> received_request_id_to_identifier_;

  std::jthread send_thread_;
  std::jthread recv_thread_;
  bool stopped_ = false;
  std::latch quit_latch_;
  exec::async_scope scope_;

  using TimePoint = std::chrono::time_point<std::chrono::high_resolution_clock>;
  TimePoint last_heartbeat_;
  std::unordered_map<uint32_t, TimePoint> last_seen_;
};

}  // namespace ex_actor::internal::network
