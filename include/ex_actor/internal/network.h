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
#include <zmq_addon.hpp>

#include "ex_actor/internal/constants.h"
#include "ex_actor/internal/local_actor_ref.h"
#include "ex_actor/internal/message.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {
struct NodeInfo {
  /// Unique ID for this node, should be unique within the cluster.
  uint32_t node_id = 0;
  /// Format: "<protocol>://<IP>:<port>". For the current node, we'll open a listener on this address. For other nodes,
  /// we'll connect to this address.
  std::string address;
};

struct NetworkConfig {
  /// How long (ms) should we consider a node dead if we haven't received any messages from it
  uint64_t heartbeat_timeout_ms = internal::kDefaultHeartbeatTimeoutMs;
  /// Interval (ms) at which gossip messages are sent.
  uint64_t gossip_interval_ms = internal::kDefaultGossipIntervalMs;
  /// Number of peers to propagate each gossip message to per round.
  size_t gossip_fanout = internal::kDefaultGossipFanout;
};

struct ClusterConfig {
  NodeInfo this_node;
  /// If you are the first node in the cluster, leave it empty. Otherwise, set it to any other node in the cluster.
  NodeInfo contact_node;
  NetworkConfig network_config;
};
}  // namespace ex_actor

namespace ex_actor::internal {

enum class MessageFlag : uint8_t { kNormal = 0, kGossip };

struct Identifier {
  uint32_t request_node_id;
  uint32_t response_node_id;
  uint64_t request_id;
  MessageFlag flag;
};

struct NodeAlivenessWaiter {
  explicit NodeAlivenessWaiter(uint64_t deadline_ms) : sem(1), deadline_ms(deadline_ms) {}
  ex_actor::Semaphore sem;
  uint64_t deadline_ms;
  bool arrived = false;
};

/**
 * @brief Pulls messages from a ZMQ recv socket on a dedicated thread and invokes a callback.
 */
class RecvSocketPuller {
 public:
  using Callback = std::function<void(zmq::multipart_t)>;

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

  explicit MessageBroker(const ClusterConfig& cluster_config);
  ~MessageBroker();

  /**
   * @brief Called by the framework after the actor is spawned to inject the self actor ref.
   */
  void OnSpawned(LocalActorRef<MessageBroker> self_actor_ref);

  /**
   * @brief Start the recv socket puller and periodical task scheduler.
   * @param request_handler Called to process incoming network requests and produce a response.
   */
  void Start(const std::string& address, RequestHandler request_handler);

  /**
   * @brief Stop the RecvSocketPuller and PeriodicalTaskScheduler, then wait for all in-flight tasks to complete.
   */
  exec::task<void> Stop();

  /**
   * @brief Wait for a node to be alive.
   * @returns True if the node is alive before the timeout, false otherwise.
   */
  exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms);

  /**
   * @brief Send buffer to the remote node and get a response.
   * @return A task containing raw response buffer.
   */
  exec::task<ByteBuffer> SendRequest(uint32_t to_node_id, ByteBuffer data);

  // Called by RecvSocketPuller
  exec::task<void> DispatchReceivedMessage(zmq::multipart_t multi);

  // ------------- periodical tasks scheduled in PeriodicalTaskScheduler -------------
  void BroadcastGossip();
  void CheckHeartbeatTimeout();
  void CheckNodeAlivenessWaiterTimeout();

 private:
  void StartRecvSocketPuller(const std::string& address);
  void StartPeriodicalTaskScheduler();

  std::vector<uint32_t> GetRandomPeers(size_t fanout);

  void HandleRepliedResponse(uint64_t request_id_in_node, ByteBuffer data);
  exec::task<void> HandleIncomingRequest(Identifier identifier, ByteBuffer data);
  void HandleGossipMessage(const ByteBuffer& gossip_data);

  void OnNodeAlive(uint32_t node_id);
  void OnNodeDead(uint32_t node_id);

  uint64_t SendToNode(uint32_t node_id, MessageFlag flag, ByteBuffer data);
  void SendReply(const Identifier& identifier, ByteBuffer data);

  struct OutstandingRequest {
    Semaphore sem;
    ByteBuffer response_bytes;
    std::exception_ptr exception_ptr;
    uint32_t response_node_id {};
    OutstandingRequest() : sem(1) {}
  };

  struct ReplyOperation {
    Identifier identifier;
    ByteBuffer data;
  };

  NodeInfo this_node_ {};
  NetworkConfig network_config_;
  uint64_t send_request_id_counter_ = 0;

  std::unordered_map</*node_id*/ uint32_t, std::vector<ReplyOperation>> deferred_replies_;
  std::unordered_map</*request_id*/ uint64_t, OutstandingRequest> outstanding_requests_;

  bool stopped_ = false;

  std::unordered_map<uint32_t, NodeState> node_id_to_state_;
  std::unordered_map<uint32_t, std::list<NodeAlivenessWaiter>> node_id_to_waiters_;
  std::mt19937 rng_ {std::random_device {}()};

  zmq::context_t zmq_context_ {/*io_threads_=*/1};
  std::unordered_map<uint32_t, zmq::socket_t> node_id_to_send_socket_;

  LocalActorRef<MessageBroker> self_actor_ref_;
  RequestHandler request_handler_;
  exec::async_scope async_scope_;
  std::unique_ptr<RecvSocketPuller> recv_socket_puller_;
  std::unique_ptr<PeriodicalTaskScheduler> periodical_task_scheduler_;
};

}  // namespace ex_actor::internal
