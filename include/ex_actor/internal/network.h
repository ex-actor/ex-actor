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

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <zmq.hpp>

#include "ex_actor/internal/constants.h"
#include "ex_actor/internal/local_actor_ref.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {

struct NetworkConfig {
  /// How long (ms) should we consider a node dead if we haven't received any messages from it
  uint64_t heartbeat_timeout_ms = internal::kDefaultHeartbeatTimeoutMs;
  /// Interval (ms) at which gossip messages are sent.
  uint64_t gossip_interval_ms = internal::kDefaultGossipIntervalMs;
  /// Number of peers to propagate each gossip message to per round.
  size_t gossip_fanout = internal::kDefaultGossipFanout;
};

struct ClusterConfig {
  std::string listen_address;
  std::string contact_node_address;
  NetworkConfig network_config;
};

struct NodeInfo {
  uint64_t node_id = 0;
  std::string address;
};

// now it only has one field, but we keep it as a separate struct for future extensibility
struct ClusterState {
  std::vector<NodeInfo> nodes;
};

/// Thrown when a remote operation fails due to the target node being
/// unreachable (dead, timed-out heartbeat, connection refused, etc.).
struct ConnectionLost : public std::runtime_error {
  uint64_t node_id;

  explicit ConnectionLost(uint64_t node_id, const std::string& message)
      : std::runtime_error(message), node_id(node_id) {}
};

struct WaitClusterStateResult {
  ClusterState cluster_state;
  bool condition_met = false;
};

}  // namespace ex_actor

namespace ex_actor::internal {

struct ClusterStateWaiter {
  explicit ClusterStateWaiter(uint64_t deadline_ms) : sem(1), deadline_ms(deadline_ms) {}
  ex_actor::Semaphore sem;
  uint64_t deadline_ms;
  bool condition_met = false;
  std::function<bool(const ClusterState&)> predicate;
};

/**
 * @brief Pulls messages from a ZMQ recv socket on a dedicated thread and invokes a callback.
 */
class RecvSocketPuller {
 public:
  using Callback = std::function<void(ByteBuffer)>;

  RecvSocketPuller(zmq::socket_t recv_socket, Callback callback);
  ~RecvSocketPuller();
  void Stop();

 private:
  void Loop(const std::stop_token& stop_token);

  zmq::socket_t recv_socket_;
  Callback callback_;
  std::jthread thread_;
  std::atomic_bool stopped_ = false;
};

/**
 * @brief Runs periodic tasks on a dedicated thread. The tasks are expected to execute quickly, won't block for too
 * long. Or the time will be not accurate.
 *
 * Tasks are kept in a min-heap ordered by next_run_ms so the thread sleeps
 * exactly until the earliest task is due.
 */
class PeriodicalTaskScheduler {
 public:
  PeriodicalTaskScheduler();
  ~PeriodicalTaskScheduler();

  void Register(std::function<void()> fn, uint64_t interval_ms);
  void Stop();

 private:
  struct Task {
    std::function<void()> fn;
    uint64_t interval_ms;
    uint64_t next_run_ms;

    bool operator>(const Task& other) const { return next_run_ms > other.next_run_ms; }
  };

  void Loop(const std::stop_token& stop_token);

  std::priority_queue<Task, std::vector<Task>, std::greater<>> tasks_;
  std::mutex tasks_mutex_;
  std::condition_variable cv_;
  std::jthread thread_;
  std::atomic_bool stopped_ = false;
};

/**
 * @brief The network message broker, designed to be used as an Actor, so no locks are needed for the state.
 */
class MessageBroker {
 public:
  using RequestHandler = std::function<exec::task<ByteBuffer>(ByteBuffer)>;

  explicit MessageBroker(uint64_t this_node_id, ClusterConfig cluster_config);
  ~MessageBroker();

  /**
   * @brief Called by the framework after the actor is spawned to inject the self actor ref.
   */
  void OnSpawned(LocalActorRef<MessageBroker> self_actor_ref);

  /**
   * @brief Start the recv socket puller and periodical task scheduler.
   * @param request_handler Called to process incoming network requests and produce a response.
   */
  void Start(RequestHandler request_handler);

  /**
   * @brief Stop the RecvSocketPuller and PeriodicalTaskScheduler, then wait for all in-flight tasks to complete.
   */
  exec::task<void> Stop();

  /** @copydoc ex_actor::WaitClusterState */
  exec::task<WaitClusterStateResult> WaitClusterState(std::function<bool(const ClusterState&)> predicate,
                                                      uint64_t timeout_ms);

  /**
   * @brief Send buffer to the remote node and get a response.
   * @return A task containing raw response buffer.
   */
  exec::task<ByteBuffer> SendRequest(uint64_t to_node_id, ByteBuffer data);

  // Called by RecvSocketPuller
  exec::task<void> DispatchReceivedMessage(ByteBuffer raw);

  // ------------- periodical tasks scheduled in PeriodicalTaskScheduler -------------
  void BroadcastGossip();
  void CheckHeartbeatTimeout();
  void CheckClusterStateWaiterTimeout();

 private:
  void StartRecvSocketPuller();
  void StartPeriodicalTaskScheduler();

  std::vector<uint64_t> GetRandomPeers(size_t fanout);

  void HandleRepliedResponse(BrokerTwoWayMessage response_msg);
  exec::task<void> HandleIncomingRequest(BrokerTwoWayMessage request_msg);
  void HandleGossipMessage(const BrokerGossipMessage& gossip_message);

  void OnNodeAlive(uint64_t node_id);
  void OnNodeConnectionLost(uint64_t node_id);

  std::vector<NodeInfo> BuildAliveNodeInfoList() const;
  void EvaluateClusterStateWaiters();

  uint64_t SendTwoWayMessage(uint64_t node_id, ByteBuffer data);
  void SendReply(uint64_t request_node_id, uint64_t request_id, ByteBuffer data);

  struct OutstandingRequest {
    Semaphore sem;
    ByteBuffer response_bytes;
    std::exception_ptr exception_ptr;
    uint64_t response_node_id {};
    OutstandingRequest() : sem(1) {}
  };

  struct ReplyOperation {
    uint64_t request_node_id;
    uint64_t request_id;
    ByteBuffer data;
  };

  uint64_t this_node_id_;
  ClusterConfig cluster_config_;
  uint64_t send_request_id_counter_ = 0;

  std::unordered_map</*node_id*/ uint64_t, std::vector<ReplyOperation>> deferred_replies_;
  std::unordered_map</*request_id*/ uint64_t, OutstandingRequest> outstanding_requests_;

  bool stopped_ = false;

  std::unordered_map<uint64_t, NodeState> node_id_to_state_;
  std::list<ClusterStateWaiter> cluster_state_waiters_;
  std::mt19937_64 rng_ {std::random_device {}()};

  zmq::context_t zmq_context_ {/*io_threads_=*/1};
  // connected at start, then moved to node_id_to_send_socket_ once got gossip from it
  std::optional<zmq::socket_t> contact_node_send_socket_;
  std::unordered_map<uint64_t, zmq::socket_t> node_id_to_send_socket_;

  LocalActorRef<MessageBroker> self_actor_ref_;
  RequestHandler request_handler_;
  exec::async_scope async_scope_;
  std::unique_ptr<RecvSocketPuller> recv_socket_puller_;
  std::unique_ptr<PeriodicalTaskScheduler> periodical_task_scheduler_;
};

}  // namespace ex_actor::internal
