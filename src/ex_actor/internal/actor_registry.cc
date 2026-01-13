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

#include "ex_actor/internal/actor_registry.h"

#include <exception>

#include "ex_actor/internal/remote_handler_registry.h"

namespace ex_actor::internal {

// ----------------------ActorRegistryRequestProcessor--------------------------
ActorRegistryRequestProcessor::ActorRegistryRequestProcessor(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                                             uint32_t this_node_id,
                                                             const std::vector<NodeInfo>& cluster_node_info,
                                                             network::MessageBroker* message_broker)
    : is_distributed_mode_(!cluster_node_info.empty()),
      scheduler_(std::move(scheduler)),
      this_node_id_(this_node_id),
      message_broker_(message_broker) {
  InitRandomNumGenerator();
  ValidateNodeInfo(cluster_node_info);
}

exec::task<void> ActorRegistryRequestProcessor::AsyncDestroyAllActors() {
  exec::async_scope async_scope;
  // bulk destroy actors
  logging::Info("Sending destroy messages to actors");
  for (auto& [_, actor] : actor_id_to_actor_) {
    async_scope.spawn(actor->AsyncDestroy());
  }
  logging::Info("Waiting for actors to be destroyed");
  co_await async_scope.on_empty();
  logging::Info("All actors destroyed");
}

exec::task<void> ActorRegistryRequestProcessor::HandleNetworkRequest(uint64_t received_request_id,
                                                                     network::ByteBufferType request_buffer) {
  serde::BufferReader<network::ByteBufferType> reader(std::move(request_buffer));
  auto message_type = reader.NextPrimitive<serde::NetworkRequestType>();

  if (message_type == serde::NetworkRequestType::kActorCreationRequest) {
    HandleActorCreationRequest(received_request_id, std::move(reader));
    co_return;
  }

  if (message_type == serde::NetworkRequestType::kActorMethodCallRequest) {
    co_await HandleActorMethodCallRequest(received_request_id, std::move(reader));
    co_return;
  }

  if (message_type == serde::NetworkRequestType::kActorLookUpRequest) {
    auto actor_name =
        serde::Deserialize<serde::ActorLookUpRequest>(reader.Current(), reader.RemainingSize()).actor_name;

    if (actor_name_to_id_.contains(actor_name)) {
      serde::BufferWriter<network::ByteBufferType> writer(
          network::ByteBufferType(sizeof(serde::NetworkRequestType) + sizeof(uint64_t)));
      auto actor_id = actor_name_to_id_.at(actor_name);
      writer.WritePrimitive(serde::NetworkReplyType::kActorLookUpReturn);
      writer.WritePrimitive(actor_id);
      message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
    } else {
      serde::BufferWriter writer(network::ByteBufferType(sizeof(serde::NetworkRequestType)));
      writer.WritePrimitive(serde::NetworkReplyType::kActorLookUpError);
      message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
    }
    co_return;
  }
  EXA_THROW << "Invalid message type: " << static_cast<int>(message_type);
}

void ActorRegistryRequestProcessor::InitRandomNumGenerator() {
  std::random_device rd;
  random_num_generator_ = std::mt19937(rd());
}

uint64_t ActorRegistryRequestProcessor::GenerateRandomActorId() {
  while (true) {
    auto id = random_num_generator_();
    if (!actor_id_to_actor_.contains(id)) {
      return id;
    }
  }
}

void ActorRegistryRequestProcessor::ValidateNodeInfo(const std::vector<NodeInfo>& cluster_node_info) {
  for (const auto& node : cluster_node_info) {
    EXA_THROW_CHECK(!node_id_to_address_.contains(node.node_id)) << "Duplicate node id: " << node.node_id;
    node_id_to_address_[node.node_id] = node.address;
  }
}

serde::NetworkRequestType ActorRegistryRequestProcessor::ParseMessageType(const network::ByteBufferType& buffer) {
  EXA_THROW_CHECK_LE(buffer.size(), 1) << "Invalid buffer size, " << buffer.size();
  return static_cast<serde::NetworkRequestType>(*static_cast<const uint8_t*>(buffer.data()));
}

void ActorRegistryRequestProcessor::ReplyError(uint64_t received_request_id, serde::NetworkReplyType reply_type,
                                               std::string error_msg) {
  std::vector<char> serialized = serde::Serialize(serde::ActorMethodReturnError {std::move(error_msg)});
  serde::BufferWriter writer(network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
  writer.WritePrimitive(reply_type);
  writer.CopyFrom(serialized.data(), serialized.size());
  message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
}

void ActorRegistryRequestProcessor::HandleActorCreationRequest(uint64_t received_request_id,
                                                               serde::BufferReader<network::ByteBufferType> reader) {
  auto handler_key_len = reader.NextPrimitive<uint64_t>();
  auto handler_key = reader.PullString(handler_key_len);
  try {
    auto handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorCreationHandler(handler_key);
    ActorRefDeserializationInfo info {.this_node_id = this_node_id_,
                                      .actor_look_up_fn = [&](uint64_t actor_id) -> TypeErasedActor* {
                                        if (actor_id_to_actor_.contains(actor_id)) {
                                          return actor_id_to_actor_.at(actor_id).get();
                                        }
                                        return nullptr;
                                      },
                                      .message_broker = message_broker_};
    auto result = handler(RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext {
        .request_buffer = std::move(reader), .scheduler = scheduler_->Clone(), .info = info});
    uint64_t actor_id = GenerateRandomActorId();
    if (result.actor_name.has_value()) {
      EXA_THROW_CHECK(!actor_name_to_id_.contains(result.actor_name.value()))
          << "An actor with the same name already exists, name=" << result.actor_name.value();
      actor_name_to_id_[result.actor_name.value()] = actor_id;
    }
    actor_id_to_actor_[actor_id] = std::move(result.actor);
    serde::BufferWriter<network::ByteBufferType> writer(
        network::ByteBufferType(sizeof(serde::NetworkReplyType) + sizeof(actor_id)));
    writer.WritePrimitive(serde::NetworkReplyType::kActorCreationReturn);
    writer.WritePrimitive(actor_id);
    message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, serde::NetworkReplyType::kActorCreationError, std::move(error_msg));
  }
}

exec::task<void> ActorRegistryRequestProcessor::HandleActorMethodCallRequest(
    uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader) {
  auto handler_key_len = reader.NextPrimitive<uint64_t>();
  auto handler_key = reader.PullString(handler_key_len);
  auto actor_id = reader.NextPrimitive<uint64_t>();
  if (!actor_id_to_actor_.contains(actor_id)) {
    ReplyError(
        received_request_id, serde::NetworkReplyType::kActorMethodCallError,
        fmt_lib::format("Can't find actor at remote node, actor_id={}, node_id={}, maybe it's already destroyed.",
                        actor_id, this_node_id_));
    co_return;
  }

  RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandler handler = nullptr;
  try {
    handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorMethodCallHandler(handler_key);
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, serde::NetworkReplyType::kActorMethodCallError, std::move(error_msg));
    co_return;
  }

  EXA_THROW_CHECK(handler != nullptr);
  ActorRefDeserializationInfo info {.this_node_id = this_node_id_,
                                    .actor_look_up_fn = [&](uint64_t actor_id) -> TypeErasedActor* {
                                      if (actor_id_to_actor_.contains(actor_id)) {
                                        return actor_id_to_actor_.at(actor_id).get();
                                      }
                                      return nullptr;
                                    },
                                    .message_broker = message_broker_};
  try {
    auto task = handler(RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext {
        .actor = actor_id_to_actor_.at(actor_id).get(), .request_buffer = std::move(reader), .info = info});
    auto buffer = co_await std::move(task);
    message_broker_->ReplyRequest(received_request_id, std::move(buffer));
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, serde::NetworkReplyType::kActorMethodCallError, std::move(error_msg));
  }
}

// ----------------------ActorRegistry--------------------------
ActorRegistry::~ActorRegistry() {
  logging::Info("Start to shutdown actor registry");
  if (is_distributed_mode_) {
    message_broker_->ClusterAlignedStop();
  }
  ex::sync_wait(processor_actor_.CallActorMethod<&ActorRegistryRequestProcessor::AsyncDestroyAllActors>());
  ex::sync_wait(processor_actor_.AsyncDestroy());
  ex::sync_wait(async_scope_.on_empty());
  logging::Info("Actor registry shutdown completed");
}

ActorRegistry::ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                             uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                             network::HeartbeatConfig heartbeat_config)
    : is_distributed_mode_(!cluster_node_info.empty()),
      this_node_id_(this_node_id),
      default_work_sharing_thread_pool_(thread_pool_size),
      scheduler_(scheduler != nullptr ? std::move(scheduler)
                                      : std::make_unique<AnyStdExecScheduler<WorkSharingThreadPool::Scheduler>>(
                                            default_work_sharing_thread_pool_.GetScheduler())),
      message_broker_([&cluster_node_info, &heartbeat_config, this]() -> std::unique_ptr<network::MessageBroker> {
        if (cluster_node_info.empty()) {
          return nullptr;
        }
        return std::make_unique<network::MessageBroker>(
            cluster_node_info, this_node_id_,
            /*request_handler=*/
            [this](uint64_t received_request_id, network::ByteBufferType data) {
              auto task = processor_actor_.CallActorMethod<&ActorRegistryRequestProcessor::HandleNetworkRequest>(
                  received_request_id, std::move(data));
              async_scope_.spawn(std::move(task));
            },
            heartbeat_config);
      }()),
      processor_actor_(scheduler_->Clone(), ActorConfig {.node_id = this_node_id_}, scheduler_->Clone(), this_node_id,
                       cluster_node_info, message_broker_.get()),
      processor_actor_ref_(this_node_id_, this_node_id_, /*actor_id=*/UINT64_MAX, &processor_actor_,
                           message_broker_.get()) {}
}  // namespace ex_actor::internal

// ----------------------Global Default Registry--------------------------

namespace {
std::vector<std::shared_ptr<void>> resource_holder;
std::unique_ptr<ex_actor::ActorRegistry> global_default_registry;
}  // namespace

namespace ex_actor::internal {
ex_actor::ActorRegistry& GetGlobalDefaultRegistry() {
  EXA_THROW_CHECK(IsGlobalDefaultRegistryInitialized()) << "Global default registry is not initialized.";
  return *global_default_registry;
}

void AssignGlobalDefaultRegistry(std::unique_ptr<ex_actor::ActorRegistry> registry) {
  global_default_registry = std::move(registry);
}

bool IsGlobalDefaultRegistryInitialized() { return global_default_registry != nullptr; }

/*
 * ThreadLocalCleanupSentinel: A thread_local object used for automatic cleanup.
 *
 * Problem: The MPSC_queue uses thread_local storage (MPSC_thread_hook). During program exit,
 * thread_local destructors run BEFORE atexit handlers (in glibc). So using atexit for cleanup
 * causes use-after-free: the MPSC_queue's thread_local data is freed, then atexit calls
 * Shutdown() which tries to use the freed data.
 *
 * Solution: Thread_local objects are destroyed in reverse order of construction within a thread.
 * The MPSC_queue's thread_local Hook is constructed during ActorRegistry construction (when
 * the first message queue is used). If we create our cleanup sentinel AFTER the ActorRegistry
 * is constructed, our destructor runs BEFORE the MPSC_queue's thread_local destructor.
 *
 * Construction order:
 *   1. MPSC_queue's thread_local Hook (during ActorRegistry ctor -> Actor ctor -> mailbox init)
 *   2. ThreadLocalCleanupSentinel (after ActorRegistry is fully constructed, during Init())
 *
 * Destruction order (reverse):
 *   1. ThreadLocalCleanupSentinel destructor -> calls Shutdown()
 *   2. MPSC_queue's thread_local Hook destructor (registry already shut down, no issue)
 */
struct ThreadLocalCleanupSentinel {
  ~ThreadLocalCleanupSentinel() {
    if (IsGlobalDefaultRegistryInitialized()) {
      Shutdown();
    }
  }
};

// Must be called AFTER ActorRegistry is constructed to ensure correct destruction order
void RegisterThreadLocalCleanup() {
  // Static thread_local ensures one instance per thread, initialized on first call
  static thread_local ThreadLocalCleanupSentinel sentinel;
  // Force sentinel construction by using it (prevent optimization from removing it)
  (void)sentinel;
}

void SetupGlobalHandlers() { logging::InstallFallbackExceptionHandler(); }
}  // namespace ex_actor::internal

namespace ex_actor {
void Init(uint32_t thread_pool_size) {
  internal::logging::Info("Initializing ex_actor in single-node mode with default scheduler, thread_pool_size={}",
                          thread_pool_size);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  internal::SetupGlobalHandlers();
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size);
  // Register cleanup AFTER ActorRegistry construction to ensure correct thread_local destruction order
  internal::RegisterThreadLocalCleanup();
}

void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::logging::Info(
      "Initializing ex_actor in distributed mode with default scheduler, thread_pool_size={}, this_node_id={}, "
      "total_nodes={}",
      thread_pool_size, this_node_id, cluster_node_info.size());
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  internal::SetupGlobalHandlers();
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size, this_node_id, cluster_node_info);
  // Register cleanup AFTER ActorRegistry construction to ensure correct thread_local destruction order
  internal::RegisterThreadLocalCleanup();
}

void HoldResource(std::shared_ptr<void> resource) { resource_holder.push_back(std::move(resource)); }

void Shutdown() {
  internal::logging::Info("Shutting down ex_actor.");
  EXA_THROW_CHECK(internal::IsGlobalDefaultRegistryInitialized()) << "Not initialized.";
  global_default_registry.reset();
  resource_holder.clear();
}

void ConfigureLogging(const logging::LogConfig& config) {
  internal::logging::GlobalLogger() = internal::logging::CreateLoggerUsingConfig(config);
}

}  // namespace ex_actor