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

#include <cstdint>
#include <exception>
#include <unordered_set>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/remote_handler_registry.h"

namespace ex_actor::internal {

// ----------------------ActorRegistryBackend--------------------------
ActorRegistryBackend::ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler, uint32_t this_node_id,
                                           const std::vector<NodeInfo>& cluster_node_info,
                                           MessageBroker* message_broker)
    : is_distributed_mode_(!cluster_node_info.empty()),
      scheduler_(std::move(scheduler)),
      this_node_id_(this_node_id),
      message_broker_(message_broker) {
  InitRandomNumGenerator();
  ValidateNodeInfo(cluster_node_info);
}

ActorRegistryBackend::ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                           const ClusterConfig& cluster_config, MessageBroker* message_broker)
    : is_distributed_mode_(true),
      scheduler_(std::move(scheduler)),
      this_node_id_(cluster_config.this_node.node_id),
      message_broker_(message_broker) {
  InitRandomNumGenerator();
  if (!cluster_config.contact_node.address.empty()) {
    EXA_THROW_CHECK(cluster_config.this_node.node_id != cluster_config.contact_node.node_id)
        << "Duplicate node id:" << cluster_config.this_node.node_id;
  }
}

exec::task<void> ActorRegistryBackend::AsyncDestroyAllActors() {
  exec::async_scope async_scope;
  // bulk destroy actors
  log::Info("Sending destroy messages to actors");
  for (auto& [_, actor] : actor_id_to_actor_) {
    async_scope.spawn(actor->AsyncDestroy());
  }
  log::Info("Waiting for actors to be destroyed");
  co_await async_scope.on_empty();
  log::Info("All actors destroyed");
}

exec::task<void> ActorRegistryBackend::HandleNetworkRequest(uint64_t received_request_id,
                                                            ByteBufferType request_buffer) {
  BufferReader<ByteBufferType> reader(std::move(request_buffer));
  auto message_type = reader.NextPrimitive<NetworkRequestType>();

  if (message_type == NetworkRequestType::kActorCreationRequest) {
    HandleActorCreationRequest(received_request_id, std::move(reader));
    co_return;
  }

  if (message_type == NetworkRequestType::kActorMethodCallRequest) {
    co_await HandleActorMethodCallRequest(received_request_id, std::move(reader));
    co_return;
  }

  if (message_type == NetworkRequestType::kActorLookUpRequest) {
    auto actor_name = Deserialize<ActorLookUpRequest>(reader.Current(), reader.RemainingSize()).actor_name;

    if (actor_name_to_id_.contains(actor_name)) {
      BufferWriter<ByteBufferType> writer(ByteBufferType(sizeof(NetworkRequestType) + sizeof(uint64_t)));
      auto actor_id = actor_name_to_id_.at(actor_name);
      writer.WritePrimitive(NetworkReplyType::kActorLookUpReturn);
      writer.WritePrimitive(actor_id);
      message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
    } else {
      BufferWriter writer(ByteBufferType(sizeof(NetworkRequestType)));
      writer.WritePrimitive(NetworkReplyType::kActorLookUpError);
      message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
    }
    co_return;
  }
  EXA_THROW << "Invalid message type: " << static_cast<int>(message_type);
}

void ActorRegistryBackend::InitRandomNumGenerator() {
  std::random_device rd;
  random_num_generator_ = std::mt19937(rd());
}

uint64_t ActorRegistryBackend::GenerateRandomActorId() {
  while (true) {
    auto id = random_num_generator_();
    if (!actor_id_to_actor_.contains(id)) {
      return id;
    }
  }
}

void ActorRegistryBackend::ValidateNodeInfo(const std::vector<NodeInfo>& cluster_node_info) {
  std::unordered_set<uint32_t> set;
  for (const auto& node : cluster_node_info) {
    EXA_THROW_CHECK(!set.contains(node.node_id)) << "Duplicate node id: " << node.node_id;
    set.insert(node.node_id);
  }
}

NetworkRequestType ActorRegistryBackend::ParseMessageType(const ByteBufferType& buffer) {
  EXA_THROW_CHECK_LE(buffer.size(), 1) << "Invalid buffer size, " << buffer.size();
  return static_cast<NetworkRequestType>(*static_cast<const uint8_t*>(buffer.data()));
}

void ActorRegistryBackend::ReplyError(uint64_t received_request_id, NetworkReplyType reply_type,
                                      std::string error_msg) {
  std::vector<char> serialized = Serialize(ActorMethodReturnError {std::move(error_msg)});
  BufferWriter writer(ByteBufferType(sizeof(NetworkRequestType) + serialized.size()));
  writer.WritePrimitive(reply_type);
  writer.CopyFrom(serialized.data(), serialized.size());
  message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
}

void ActorRegistryBackend::HandleActorCreationRequest(uint64_t received_request_id,
                                                      BufferReader<ByteBufferType> reader) {
  auto handler_key_len = reader.NextPrimitive<uint64_t>();
  auto handler_key = reader.PullString(handler_key_len);
  try {
    auto handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorCreationHandler(handler_key);
    ActorRefSerdeContext info {.this_node_id = this_node_id_,
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
    BufferWriter<ByteBufferType> writer(ByteBufferType(sizeof(NetworkReplyType) + sizeof(actor_id)));
    writer.WritePrimitive(NetworkReplyType::kActorCreationReturn);
    writer.WritePrimitive(actor_id);
    message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, NetworkReplyType::kActorCreationError, std::move(error_msg));
  }
}

exec::task<void> ActorRegistryBackend::HandleActorMethodCallRequest(uint64_t received_request_id,
                                                                    BufferReader<ByteBufferType> reader) {
  auto handler_key_len = reader.NextPrimitive<uint64_t>();
  auto handler_key = reader.PullString(handler_key_len);
  auto actor_id = reader.NextPrimitive<uint64_t>();
  if (!actor_id_to_actor_.contains(actor_id)) {
    ReplyError(
        received_request_id, NetworkReplyType::kActorMethodCallError,
        fmt_lib::format("Can't find actor at remote node, actor_id={}, node_id={}, maybe it's already destroyed.",
                        actor_id, this_node_id_));
    co_return;
  }

  RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandler handler = nullptr;
  try {
    handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorMethodCallHandler(handler_key);
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    ReplyError(received_request_id, NetworkReplyType::kActorMethodCallError, std::move(error_msg));
    co_return;
  }

  EXA_THROW_CHECK(handler != nullptr);
  ActorRefSerdeContext info {.this_node_id = this_node_id_,
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
    ReplyError(received_request_id, NetworkReplyType::kActorMethodCallError, std::move(error_msg));
  }
}

exec::task<bool> ActorRegistryBackend::WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  co_return co_await message_broker_->WaitNodeAlive(node_id, timeout_ms);
}

// ----------------------ActorRegistry--------------------------
ActorRegistry::~ActorRegistry() {
  log::Info("Start to shutdown actor registry");
  if (is_distributed_mode_) {
    message_broker_->ClusterAlignedStop();
  }
  ex::sync_wait(backend_actor_.CallActorMethod<&ActorRegistryBackend::AsyncDestroyAllActors>());
  ex::sync_wait(backend_actor_.AsyncDestroy());
  ex::sync_wait(async_scope_.on_empty());
  log::Info("Actor registry shutdown completed");
}

ActorRegistry::ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                             uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                             NetworkConfig network_config)
    : is_distributed_mode_(!cluster_node_info.empty()),
      this_node_id_(this_node_id),
      default_work_sharing_thread_pool_(thread_pool_size),
      scheduler_(scheduler != nullptr ? std::move(scheduler)
                                      : std::make_unique<AnyStdExecScheduler<WorkSharingThreadPool::Scheduler>>(
                                            default_work_sharing_thread_pool_.GetScheduler())),
      message_broker_([&cluster_node_info, &network_config, this]() -> std::unique_ptr<MessageBroker> {
        if (cluster_node_info.empty()) {
          return nullptr;
        }

        NodeInfo this_node {.node_id = this_node_id_};
        NodeInfo contact_node {.node_id = this_node_id_};
        for (const auto& node : cluster_node_info) {
          if (node.node_id < contact_node.node_id) {
            contact_node.node_id = node.node_id;
            contact_node.address = node.address;
          }

          if (node.node_id == this_node_id_) {
            this_node.address = node.address;
          }
        }

        ClusterConfig cluster_config {.this_node = std::move(this_node), .network_config = network_config};
        if (cluster_config.this_node.node_id != contact_node.node_id) {
          cluster_config.contact_node = std::move(contact_node);
        }

        return std::make_unique<MessageBroker>(
            cluster_config,
            /*request_handler=*/
            [this](uint64_t received_request_id, ByteBufferType data) {
              auto task = backend_actor_.CallActorMethod<&ActorRegistryBackend::HandleNetworkRequest>(
                  received_request_id, std::move(data));
              async_scope_.spawn(std::move(task));
            });
      }()),
      backend_actor_(scheduler_->Clone(), ActorConfig {.node_id = this_node_id_}, scheduler_->Clone(), this_node_id,
                     cluster_node_info, message_broker_.get()),
      backend_actor_ref_(this_node_id_, this_node_id_, /*actor_id=*/UINT64_MAX, &backend_actor_,
                         message_broker_.get()) {
  for (const auto& node : cluster_node_info) {
    if (node.node_id != this_node_id) {
      auto [connected] = stdexec::sync_wait(WaitNodeAlive(node.node_id, 4000)).value();
      EXA_THROW_CHECK(connected) << "Can not connect to the node " << node.node_id;
    }
  }
}

ActorRegistry::ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                             const ClusterConfig& cluster_config)
    : is_distributed_mode_(),
      this_node_id_(cluster_config.this_node.node_id),
      default_work_sharing_thread_pool_(thread_pool_size),
      scheduler_(scheduler != nullptr ? std::move(scheduler)
                                      : std::make_unique<AnyStdExecScheduler<WorkSharingThreadPool::Scheduler>>(
                                            default_work_sharing_thread_pool_.GetScheduler())),
      message_broker_([&cluster_config, this]() -> std::unique_ptr<MessageBroker> {
        return std::make_unique<MessageBroker>(
            cluster_config,
            /*request_handler=*/
            [this](uint64_t received_request_id, ByteBufferType data) {
              auto task = backend_actor_.CallActorMethod<&ActorRegistryBackend::HandleNetworkRequest>(
                  received_request_id, std::move(data));
              async_scope_.spawn(std::move(task));
            });
      }()),

      backend_actor_(scheduler_->Clone(), ActorConfig {.node_id = this_node_id_}, scheduler_->Clone(), cluster_config,
                     message_broker_.get()),
      backend_actor_ref_(this_node_id_, this_node_id_, /*actor_id=*/UINT64_MAX, &backend_actor_,
                         message_broker_.get()) {}

exec::task<bool> ActorRegistry::WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  if (node_id == this_node_id_) {
    co_return true;
  }
  co_return co_await backend_actor_ref_.Send<&ActorRegistryBackend::WaitNodeAlive>(node_id, timeout_ms);
}

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

static void RegisterAtExitCleanup() {
  static bool at_exit_cleanup_registered = false;

  if (at_exit_cleanup_registered) {
    return;
  }
  at_exit_cleanup_registered = true;

  // Because Init() is called after main starts, this handler will be called before static object destruction.
  // Once we enter static object destruction without calling Shutdown(), the program will crash due to static object
  // destruction order fiasco. So we print an error message here and force exit the program.
  //
  // We can't call Shutdown() here for user, because thread_local objects are already destroyed, shutting down
  // ActorRegistry needs to use the mailbox, which has some thread_local objects, the program will crash if we call
  // Shutdown() here. Printing an error message is the best we can do here. User need to call Shutdown() explicitly
  // before main() exits.
  std::atexit([] {
    if (!IsGlobalDefaultRegistryInitialized()) {
      return;
    }
    log::Error(
        "ex_actor is not shutdown when exiting main(), calling std::quick_exit(1) to force exit, resources may not be "
        "cleaned properly. To fix this error, call ex_actor::Shutdown() before main() exits.");
    std::quick_exit(1);
  });
}

void SetupGlobalHandlers() {
  InstallFallbackExceptionHandler();
  RegisterAtExitCleanup();
}
}  // namespace ex_actor::internal

namespace ex_actor {
void Init(uint32_t thread_pool_size) {
  internal::log::Info("Initializing ex_actor in single-node mode with default scheduler, thread_pool_size={}",
                      thread_pool_size);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size);
  internal::SetupGlobalHandlers();
}

void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::log::Info(
      "Initializing ex_actor in distributed mode with default scheduler, thread_pool_size={}, this_node_id={}, "
      "total_nodes={}",
      thread_pool_size, this_node_id, cluster_node_info.size());
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size, this_node_id, cluster_node_info);
  internal::SetupGlobalHandlers();
}

void Init(uint32_t thread_pool_size, const ClusterConfig& cluster_config) {
  internal::log::Info(
      "Initializing ex_actor in distributed mode with default scheduler, thread_pool_size={}, this_node_id={}, ",
      thread_pool_size, cluster_config.this_node.node_id);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  internal::SetupGlobalHandlers();
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size, cluster_config);
}

void HoldResource(std::shared_ptr<void> resource) { resource_holder.push_back(std::move(resource)); }

exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  co_return co_await global_default_registry->WaitNodeAlive(node_id, timeout_ms);
}

void Shutdown() {
  internal::log::Info("Shutting down ex_actor.");
  EXA_THROW_CHECK(internal::IsGlobalDefaultRegistryInitialized()) << "Not initialized.";
  global_default_registry.reset();
  resource_holder.clear();
}

void ConfigureLogging(const LogConfig& config) { internal::GlobalLogger() = internal::CreateLoggerUsingConfig(config); }

}  // namespace ex_actor
