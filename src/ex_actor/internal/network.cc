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
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {

namespace {
inline uint64_t GetTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::span<const std::byte> ZmqMsgBytes(const zmq::message_t& msg) {
  return {static_cast<const std::byte*>(msg.data()), msg.size()};
}

ByteBuffer ZmqMsgToByteBuffer(zmq::message_t&& msg) {
  auto span = ZmqMsgBytes(msg);
  // TODO: a copy here, optimize it in the future
  return {span.begin(), span.end()};
}

/// non-copy, return a view of the ByteBuffer
zmq::const_buffer ByteBufferToZmqBuffer(const ByteBuffer& bytes) { return zmq::buffer(bytes.data(), bytes.size()); }

/// All sockets are configured with unlimited sndhwm, so send should never fail with EAGAIN.
void ZmqSendOrDie(zmq::socket_t& socket, zmq::const_buffer buf) {
  auto result = socket.send(buf, zmq::send_flags::dontwait);
  EXA_THROW_CHECK(result.has_value()) << "ZMQ send failed unexpectedly (sndhwm should be unlimited)";
}

}  // namespace

// ----------------------RecvSocketPuller--------------------------

RecvSocketPuller::RecvSocketPuller(zmq::socket_t recv_socket, Callback callback)
    : recv_socket_(std::move(recv_socket)), callback_(std::move(callback)) {
  thread_ = std::jthread([this](const std::stop_token& stop_token) { Loop(stop_token); });
}

RecvSocketPuller::~RecvSocketPuller() {
  if (!stopped_.load(std::memory_order_acquire)) {
    log::Critical("RecvSocketPuller destroyed without calling Stop() first");
  }
}

void RecvSocketPuller::Stop() {
  stopped_.store(true, std::memory_order_release);
  thread_.request_stop();
  thread_.join();
}

void RecvSocketPuller::Loop(const std::stop_token& stop_token) {
  SetThreadName("recv_proc_loop");
  recv_socket_.set(zmq::sockopt::rcvtimeo, 100);

  while (!stop_token.stop_requested()) {
    zmq::message_t msg;
    auto result = recv_socket_.recv(msg);
    if (!result.has_value()) {
      continue;
    }
    callback_(ZmqMsgToByteBuffer(std::move(msg)));
  }
}

// ----------------------PeriodicalTaskScheduler--------------------------

PeriodicalTaskScheduler::PeriodicalTaskScheduler() {
  thread_ = std::jthread([this](const std::stop_token& stop_token) { Loop(stop_token); });
}

PeriodicalTaskScheduler::~PeriodicalTaskScheduler() {
  if (!stopped_.load(std::memory_order_acquire)) {
    Stop();
  }
}

void PeriodicalTaskScheduler::Register(std::function<void()> fn, uint64_t interval_ms) {
  {
    std::lock_guard lock(tasks_mutex_);
    tasks_.push(Task {.fn = std::move(fn), .interval_ms = interval_ms, .next_run_ms = GetTimeMs() + interval_ms});
  }
  cv_.notify_one();
}

void PeriodicalTaskScheduler::Stop() {
  stopped_.store(true, std::memory_order_release);
  thread_.request_stop();
  cv_.notify_one();
  thread_.join();
}

void PeriodicalTaskScheduler::Loop(const std::stop_token& stop_token) {
  SetThreadName("periodical_task_loop");
  while (!stop_token.stop_requested()) {
    std::unique_lock lock(tasks_mutex_);
    if (tasks_.empty()) {
      cv_.wait(lock, [&] { return !tasks_.empty() || stop_token.stop_requested(); });
      continue;
    }

    auto next_run = std::chrono::system_clock::time_point(std::chrono::milliseconds(tasks_.top().next_run_ms));
    cv_.wait_until(lock, next_run, [&] { return stop_token.stop_requested(); });

    if (stop_token.stop_requested()) break;

    auto now = GetTimeMs();
    while (!tasks_.empty() && tasks_.top().next_run_ms <= now) {
      // the only way to move from priority_queue is to cast to non-const reference and move from it, it's a well-known
      // pattern, no worries about constness.
      auto task = std::move(const_cast<Task&>(tasks_.top()));
      tasks_.pop();
      task.fn();
      task.next_run_ms = now + task.interval_ms;
      tasks_.push(std::move(task));
    }
  }
}

// ----------------------MessageBroker--------------------------

MessageBroker::MessageBroker(uint64_t this_node_id, ClusterConfig cluster_config)
    : this_node_id_(this_node_id), cluster_config_(std::move(cluster_config)) {
  EXA_THROW_CHECK(!cluster_config_.listen_address.empty()) << "listen_address must not be empty";
  node_id_to_state_[this_node_id_] = {
      .last_seen_timestamp_ms = GetTimeMs(), .node_id = this_node_id_, .address = cluster_config_.listen_address};
  if (!cluster_config_.contact_node_address.empty()) {
    EXA_THROW_CHECK_NE(cluster_config_.contact_node_address, cluster_config_.listen_address);
    contact_node_send_socket_ = zmq::socket_t(zmq_context_, zmq::socket_type::dealer);
    contact_node_send_socket_.set(zmq::sockopt::linger, 0);
    contact_node_send_socket_.set(zmq::sockopt::sndhwm, 0);
    contact_node_send_socket_.connect(cluster_config_.contact_node_address);
  }
}

MessageBroker::~MessageBroker() {
  if (!stopped_) {
    log::Critical("MessageBroker destroyed without calling Stop() first, node_id={:#x}", this_node_id_);
  }
}

void MessageBroker::OnSpawned(LocalActorRef<MessageBroker> self_actor_ref) { self_actor_ref_ = self_actor_ref; }

void MessageBroker::Start(RequestHandler request_handler) {
  EXA_THROW_CHECK(!self_actor_ref_.IsEmpty()) << "OnSpawned() must be called before Start()";
  EXA_THROW_CHECK(request_handler != nullptr) << "request_handler must not be null";
  request_handler_ = std::move(request_handler);
  StartRecvSocketPuller();
  StartPeriodicalTaskScheduler();
}

exec::task<void> MessageBroker::Stop() {
  log::Info("Node {:#x} stopping message broker", this_node_id_);
  if (periodical_task_scheduler_ != nullptr) {
    periodical_task_scheduler_->Stop();
  }
  if (recv_socket_puller_ != nullptr) {
    recv_socket_puller_->Stop();
  }
  log::Info("Node {:#x}'s message broker stopped, waiting for in-flight tasks", this_node_id_);
  co_await async_scope_.on_empty();
  log::Info("Node {:#x}'s message broker fully stopped", this_node_id_);
  stopped_ = true;
}

exec::task<void> MessageBroker::DispatchReceivedMessage(ByteBuffer raw) {
  auto broker_msg = Deserialize<BrokerMessage>(raw);

  if (auto* gossip = std::get_if<BrokerGossipMessage>(&broker_msg.variant)) {
    HandleGossipMessage(*gossip);
    co_return;
  }
  auto& two_way = std::get<BrokerTwoWayMessage>(broker_msg.variant);
  if (two_way.request_node_id == this_node_id_) {
    HandleRepliedResponse(std::move(two_way));
  } else if (two_way.response_node_id == this_node_id_) {
    co_await HandleIncomingRequest(std::move(two_way));
  } else {
    EXA_THROW << "Received two-way message not addressed to this node";
  }
}

exec::task<ByteBuffer> MessageBroker::SendRequest(uint64_t to_node_id, ByteBuffer data) {
  EXA_THROW_CHECK_NE(to_node_id, this_node_id_) << "Cannot send message to current node";
  uint64_t request_id = SendTwoWayMessage(to_node_id, std::move(data));
  auto [iter, inserted] = outstanding_requests_.try_emplace(request_id);
  EXA_THROW_CHECK(inserted);
  auto& outstanding_request = iter->second;
  outstanding_request.response_node_id = to_node_id;

  co_await outstanding_request.sem.OnDrained();

  auto response_bytes = std::move(outstanding_request.response_bytes);
  auto exception_ptr = outstanding_request.exception_ptr;
  outstanding_requests_.erase(iter);

  if (exception_ptr) {
    std::rethrow_exception(exception_ptr);
  }
  co_return std::move(response_bytes);
}

exec::task<WaitClusterStateResult> MessageBroker::WaitClusterState(std::function<bool(const ClusterState&)> predicate,
                                                                   uint64_t timeout_ms) {
  ClusterState state {.nodes = BuildAliveNodeInfoList()};
  if (predicate(state)) {
    co_return WaitClusterStateResult {.cluster_state = std::move(state), .condition_met = true};
  }

  auto waiter_it = cluster_state_waiters_.emplace(cluster_state_waiters_.end(), GetTimeMs() + timeout_ms);
  waiter_it->predicate = std::move(predicate);

  co_await waiter_it->sem.OnDrained();
  bool met = waiter_it->condition_met;
  cluster_state_waiters_.erase(waiter_it);

  co_return WaitClusterStateResult {.cluster_state = {.nodes = BuildAliveNodeInfoList()}, .condition_met = met};
}

std::vector<uint64_t> MessageBroker::GetRandomPeers(size_t fanout) {
  EXA_THROW_CHECK_GT(fanout, 0);

  // get all alive nodes except ourselves
  std::vector<uint64_t> node_ids;
  node_ids.reserve(node_id_to_state_.size());
  for (const auto& [node_id, node_state] : node_id_to_state_) {
    if (node_id != this_node_id_) {
      node_ids.emplace_back(node_id);
    }
  }

  // shuffle and truncate to fanout size
  std::shuffle(node_ids.begin(), node_ids.end(), rng_);
  if (fanout < node_ids.size()) {
    node_ids.resize(fanout);
  }
  return node_ids;
}

void MessageBroker::BroadcastGossip() {
  // update last seen timestamp for ourselves to current time
  MapAt(node_id_to_state_, this_node_id_).last_seen_timestamp_ms = GetTimeMs();

  // broadcast all known node states(include ourselves) to random peers
  std::vector<uint64_t> node_ids = GetRandomPeers(cluster_config_.network_config.gossip_fanout);
  BrokerGossipMessage gossip_message;
  gossip_message.from_node_id = this_node_id_;
  gossip_message.node_states.reserve(node_id_to_state_.size());
  for (const auto& [node_id, node_state] : node_id_to_state_) {
    gossip_message.node_states.emplace_back(node_state);
  }
  BrokerMessage broker_msg {.variant = std::move(gossip_message)};
  auto serialized = Serialize(broker_msg);

  for (uint64_t node_id : node_ids) {
    auto& node_state = MapAt(node_id_to_state_, node_id);
    auto& socket = MapAt(node_id_to_send_socket_, node_id);
    ZmqSendOrDie(socket, ByteBufferToZmqBuffer(serialized));
  }
  // no matter the contact node is in `node_id_to_state_` or not, we always send a copy to it
  if (contact_node_send_socket_.handle() != nullptr) {
    ZmqSendOrDie(contact_node_send_socket_, ByteBufferToZmqBuffer(serialized));
  }
}

void MessageBroker::CheckHeartbeatTimeout() {
  auto now_ms = GetTimeMs();
  std::vector<uint64_t> nodes_to_remove;
  for (auto& [node_id, state] : node_id_to_state_) {
    // When using system_clock, remote timestamps may be slightly ahead of local clock due to
    // clock skew across machines. Treat future timestamps as "just seen" to avoid unsigned
    // underflow in the subtraction, which would falsely trigger a heartbeat timeout.
    if (node_id == this_node_id_ ||                // ourselves
        state.last_seen_timestamp_ms >= now_ms ||  // timestamp in the future due to clock skew
        now_ms - state.last_seen_timestamp_ms < cluster_config_.network_config.heartbeat_timeout_ms  // not timeout yet
    ) {
      continue;
    }
    nodes_to_remove.push_back(node_id);
  }
  for (uint64_t node_id : nodes_to_remove) {
    OnNodeConnectionLost(node_id);
  }
}

void MessageBroker::CheckClusterStateWaiterTimeout() {
  auto now_ms = GetTimeMs();
  for (auto it = cluster_state_waiters_.begin(); it != cluster_state_waiters_.end();) {
    auto& waiter = *it;
    ++it;  // advance before signaling -- the coroutine may erase this entry on resume
    if (!waiter.condition_met && waiter.deadline_ms <= now_ms) {
      waiter.sem.Acquire(1);
    }
  }
}

void MessageBroker::StartRecvSocketPuller() {
  zmq::socket_t recv_socket {zmq_context_, zmq::socket_type::dealer};
  recv_socket.bind(cluster_config_.listen_address);
  recv_socket.set(zmq::sockopt::linger, 0);
  recv_socket.set(zmq::sockopt::sndhwm, 0);
  log::Info("Node {:#x}'s recv socket bound to {}", this_node_id_, cluster_config_.listen_address);

  recv_socket_puller_ = std::make_unique<RecvSocketPuller>(std::move(recv_socket), [this](ByteBuffer raw) {
    async_scope_.spawn(self_actor_ref_.SendLocal<&MessageBroker::DispatchReceivedMessage>(std::move(raw)));
  });
}

void MessageBroker::StartPeriodicalTaskScheduler() {
  periodical_task_scheduler_ = std::make_unique<PeriodicalTaskScheduler>();
  periodical_task_scheduler_->Register(
      [this]() { async_scope_.spawn(self_actor_ref_.SendLocal<&MessageBroker::BroadcastGossip>()); },
      cluster_config_.network_config.gossip_interval_ms);
  periodical_task_scheduler_->Register(
      [this]() { async_scope_.spawn(self_actor_ref_.SendLocal<&MessageBroker::CheckHeartbeatTimeout>()); },
      kDefaultHeartbeatCheckIntervalMs);
  periodical_task_scheduler_->Register(
      [this]() { async_scope_.spawn(self_actor_ref_.SendLocal<&MessageBroker::CheckClusterStateWaiterTimeout>()); },
      kDefaultWaiterExpirationCheckIntervalMs);
}

void MessageBroker::HandleGossipMessage(const BrokerGossipMessage& gossip_message) {
  for (const auto& incoming_node_state : gossip_message.node_states) {
    auto [iter, inserted] = node_id_to_state_.try_emplace(incoming_node_state.node_id, incoming_node_state);
    if (inserted) {
      // new alive node found
      OnNodeAlive(incoming_node_state.node_id);
      continue;
    }
    // update existing node state
    auto& cur_node_state = iter->second;
    EXA_THROW_CHECK_EQ(cur_node_state.address, incoming_node_state.address)
        << fmt_lib::format("Node {:#x} has conflicting address, {} vs {}.", cur_node_state.node_id,
                           cur_node_state.address, incoming_node_state.address);
    cur_node_state.last_seen_timestamp_ms =
        std::max(cur_node_state.last_seen_timestamp_ms, incoming_node_state.last_seen_timestamp_ms);
  }
}

void MessageBroker::HandleRepliedResponse(BrokerTwoWayMessage response_msg) {
  auto it = outstanding_requests_.find(response_msg.request_id);
  if (it == outstanding_requests_.end()) {
    log::Critical("Received response for unknown request id {}", response_msg.request_id);
    return;
  }
  auto& [request_id, pending] = *it;
  pending.response_bytes = std::move(response_msg.payload);
  pending.sem.Acquire(1);
}

exec::task<void> MessageBroker::HandleIncomingRequest(BrokerTwoWayMessage request_msg) {
  EXA_THROW_CHECK(request_handler_ != nullptr) << "Request handler not set";
  ByteBuffer reply_data = co_await request_handler_(std::move(request_msg.payload));
  SendReply(request_msg.request_node_id, request_msg.request_id, std::move(reply_data));
}

void MessageBroker::OnNodeAlive(uint64_t node_id) {
  const auto& new_node = MapAt(node_id_to_state_, node_id);
  log::Info("[Gossip] Node {:#x} found node {:#x}, connecting to it at {}", this_node_id_, new_node.node_id,
            new_node.address);

  // Create send socket
  auto& socket = (node_id_to_send_socket_[new_node.node_id] = zmq::socket_t(zmq_context_, zmq::socket_type::dealer));
  socket.set(zmq::sockopt::linger, 0);
  socket.set(zmq::sockopt::sndhwm, 0);
  socket.connect(new_node.address);

  EvaluateClusterStateWaiters();

  // flush deferred replies
  auto node_handle = deferred_replies_.extract(new_node.node_id);
  if (node_handle.empty()) {
    return;
  }
  for (auto&& reply : node_handle.mapped()) {
    SendReply(reply.request_node_id, reply.request_id, std::move(reply.data));
  }
}

void MessageBroker::OnNodeConnectionLost(uint64_t node_id) {
  // Collect matching requests first so we can log the count before signaling
  std::vector<decltype(outstanding_requests_)::iterator> requests_to_fail;
  for (auto it = outstanding_requests_.begin(); it != outstanding_requests_.end(); ++it) {
    if (it->second.response_node_id == node_id) {
      requests_to_fail.push_back(it);
    }
  }
  log::Info(
      "Node {:#x} lost connection to node {:#x} due to heartbeat timeout, {} outstanding requests of this node will "
      "be failed, heartbeat timeout is {}ms",
      this_node_id_, node_id, requests_to_fail.size(), cluster_config_.network_config.heartbeat_timeout_ms);
  for (auto it : requests_to_fail) {
    auto& [request_id, request] = *it;
    request.exception_ptr = std::make_exception_ptr(
        NetworkError(node_id, fmt_lib::format("Node {:#x} is dead, cannot complete request", node_id)));
    // this will wake up the coroutine and erase the iterator, don't use it after this
    request.sem.Acquire(1);
  }
  node_id_to_state_.erase(node_id);
  node_id_to_send_socket_.erase(node_id);

  // `deferred_replies_` should not be cleared.
  // Because the node might be alive again, the death might just be a transient network issue.

  EvaluateClusterStateWaiters();
}

uint64_t MessageBroker::SendTwoWayMessage(uint64_t node_id, ByteBuffer data) {
  auto request_id = send_request_id_counter_++;
  BrokerMessage broker_msg {.variant = BrokerTwoWayMessage {
                                .request_node_id = this_node_id_,
                                .response_node_id = node_id,
                                .request_id = request_id,
                                .payload = std::move(data),
                            }};
  auto socket_it = node_id_to_send_socket_.find(node_id);
  if (socket_it == node_id_to_send_socket_.end()) {
    throw NetworkError(node_id, fmt_lib::format("Node {:#x} is trying to send request to an unconnected node {:#x}",
                                                this_node_id_, node_id));
  }
  auto& [target_node_id, socket] = *socket_it;
  ZmqSendOrDie(socket, ByteBufferToZmqBuffer(Serialize(broker_msg)));
  return request_id;
}

void MessageBroker::SendReply(uint64_t request_node_id, uint64_t request_id, ByteBuffer data) {
  auto socket_it = node_id_to_send_socket_.find(request_node_id);
  if (socket_it == node_id_to_send_socket_.end()) {
    // It's possible due to the async nature of gossip protocol. When A discovers B via gossip, B may not discover A
    // yet. Pend the reply until we discover the peer.
    deferred_replies_[request_node_id].push_back(ReplyOperation {
        .request_node_id = request_node_id,
        .request_id = request_id,
        .data = std::move(data),
    });
    return;
  }
  auto& [target_node_id, socket] = *socket_it;
  BrokerMessage broker_msg {.variant = BrokerTwoWayMessage {
                                .request_node_id = request_node_id,
                                .response_node_id = this_node_id_,
                                .request_id = request_id,
                                .payload = std::move(data),
                            }};
  ZmqSendOrDie(socket, ByteBufferToZmqBuffer(Serialize(broker_msg)));
}

std::vector<NodeInfo> MessageBroker::BuildAliveNodeInfoList() const {
  std::vector<NodeInfo> result;
  result.reserve(node_id_to_state_.size());
  for (const auto& [node_id, state] : node_id_to_state_) {
    result.push_back(NodeInfo {.node_id = node_id, .address = state.address});
  }
  return result;
}

void MessageBroker::EvaluateClusterStateWaiters() {
  ClusterState state {.nodes = BuildAliveNodeInfoList()};
  for (auto it = cluster_state_waiters_.begin(); it != cluster_state_waiters_.end();) {
    auto& waiter = *it;
    ++it;  // advance before signaling -- the coroutine may erase this entry on resume
    if (!waiter.condition_met && waiter.predicate(state)) {
      waiter.condition_met = true;
      waiter.sem.Acquire(1);
    }
  }
}

}  // namespace ex_actor::internal
