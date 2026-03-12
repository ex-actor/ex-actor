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

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/remote_handler_registry.h"

namespace ex_actor::internal {

// ----------------------ActorRegistryBackend--------------------------
ActorRegistryBackend::ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                           const ClusterConfig& cluster_config,
                                           LocalActorRef<MessageBroker> broker_actor_ref)
    : scheduler_(std::move(scheduler)),
      this_node_id_(cluster_config.this_node.node_id),
      broker_actor_ref_(broker_actor_ref) {
  InitRandomNumGenerator();
  if (!cluster_config.contact_node.address.empty()) {
    EXA_THROW_CHECK(cluster_config.this_node.node_id != cluster_config.contact_node.node_id)
        << "Duplicate node id: " << cluster_config.this_node.node_id;
  }
}

exec::task<void> ActorRegistryBackend::AsyncDestroyAllActors() {
  exec::async_scope async_scope;
  log::Info("Sending destroy messages to actors");
  for (auto& [actor_id, actor] : actor_id_to_actor_) {
    async_scope.spawn(actor->AsyncDestroy());
  }
  log::Info("Waiting for actors to be destroyed");
  co_await async_scope.on_empty();
  log::Info("All actors destroyed");
}

exec::task<ByteBuffer> ActorRegistryBackend::HandleNetworkRequest(ByteBuffer request_buffer) {
  auto request = Deserialize<NetworkRequest>(request_buffer);

  if (auto* msg = std::get_if<ActorCreationRequest>(&request.variant)) {
    ByteBuffer reply_out;
    HandleActorCreationRequest(std::move(*msg), reply_out);
    co_return reply_out;
  }

  if (auto* msg = std::get_if<ActorMethodCallRequest>(&request.variant)) {
    co_return co_await HandleActorMethodCallRequest(std::move(*msg));
  }

  if (auto* msg = std::get_if<ActorLookUpRequest>(&request.variant)) {
    if (actor_name_to_id_.contains(msg->actor_name)) {
      co_return SerializeReply(
          NetworkReply {ActorLookUpReply {.success = true, .actor_id = actor_name_to_id_.at(msg->actor_name)}});
    } else {
      co_return SerializeReply(NetworkReply {ActorLookUpReply {.success = false}});
    }
  }

  EXA_THROW << "Unknown network request variant";
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

ByteBuffer ActorRegistryBackend::SerializeReply(const NetworkReply& reply) { return Serialize(reply); }

void ActorRegistryBackend::HandleActorCreationRequest(ActorCreationRequest msg, ByteBuffer& reply_out) {
  try {
    auto handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorCreationHandler(msg.handler_key);
    ActorRefSerdeContext info {.this_node_id = this_node_id_,
                               .actor_look_up_fn = [&](uint64_t actor_id) -> TypeErasedActor* {
                                 if (actor_id_to_actor_.contains(actor_id)) {
                                   return actor_id_to_actor_.at(actor_id).get();
                                 }
                                 return nullptr;
                               },
                               .broker_actor_ref = broker_actor_ref_};
    uint64_t actor_id = GenerateRandomActorId();
    auto result = handler(RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext {
        .serialized_args = std::move(msg.serialized_args),
        .scheduler = scheduler_->Clone(),
        .actor_ref_serde_ctx = info,
        .actor_id = actor_id});
    if (result.actor_name.has_value()) {
      EXA_THROW_CHECK(!actor_name_to_id_.contains(result.actor_name.value()))
          << "An actor with the same name already exists, name=" << result.actor_name.value();
      actor_name_to_id_[result.actor_name.value()] = actor_id;
    }
    actor_id_to_actor_[actor_id] = std::move(result.actor);
    reply_out = SerializeReply(NetworkReply {ActorCreationReply {.success = true, .actor_id = actor_id}});
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    reply_out = SerializeReply(NetworkReply {ActorCreationReply {.success = false, .error = std::move(error_msg)}});
  }
}

exec::task<ByteBuffer> ActorRegistryBackend::HandleActorMethodCallRequest(ActorMethodCallRequest msg) {
  if (!actor_id_to_actor_.contains(msg.actor_id)) {
    auto error_msg =
        fmt_lib::format("Can't find actor at remote node, actor_id={}, node_id={}, maybe it's already destroyed.",
                        msg.actor_id, this_node_id_);
    co_return SerializeReply(NetworkReply {ActorMethodCallReply {.success = false, .error = std::move(error_msg)}});
  }

  RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandler handler = nullptr;
  try {
    handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorMethodCallHandler(msg.handler_key);
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    co_return SerializeReply(NetworkReply {ActorMethodCallReply {.success = false, .error = std::move(error_msg)}});
  }

  EXA_THROW_CHECK(handler != nullptr);
  ActorRefSerdeContext info {.this_node_id = this_node_id_,
                             .actor_look_up_fn = [&](uint64_t aid) -> TypeErasedActor* {
                               if (actor_id_to_actor_.contains(aid)) {
                                 return actor_id_to_actor_.at(aid).get();
                               }
                               return nullptr;
                             },
                             .broker_actor_ref = broker_actor_ref_};
  try {
    auto task = handler(RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext {
        .actor = actor_id_to_actor_.at(msg.actor_id).get(),
        .serialized_args = std::move(msg.serialized_args),
        .actor_ref_serde_ctx = info});
    auto reply = co_await std::move(task);
    co_return SerializeReply(reply);
  } catch (std::exception& error) {
    auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
    co_return SerializeReply(NetworkReply {ActorMethodCallReply {.success = false, .error = std::move(error_msg)}});
  }
}

exec::task<bool> ActorRegistryBackend::WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty()) << "Broker actor not set";
  co_return co_await broker_actor_ref_.SendLocal<&MessageBroker::WaitNodeAlive>(node_id, timeout_ms);
}

// ----------------------ActorRegistry--------------------------
ActorRegistry::~ActorRegistry() {
  log::Info("Start to shutdown actor registry");
  // 1. Stop and destroy the message broker, then there will be no incoming requests.
  if (!broker_actor_ref_.IsEmpty()) {
    ex::sync_wait(broker_actor_ref_.SendLocal<&MessageBroker::Stop>());
    ex::sync_wait(broker_actor_->AsyncDestroy());
  }
  // 3. Destroy user actors.
  ex::sync_wait(backend_actor_ref_.SendLocal<&ActorRegistryBackend::AsyncDestroyAllActors>());
  // 5. Destroy the backend actor.
  ex::sync_wait(backend_actor_.AsyncDestroy());
  log::Info("Actor registry shutdown completed");
}

ActorRegistry::ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                             const ClusterConfig& cluster_config)
    : this_node_id_(cluster_config.this_node.node_id),
      default_work_sharing_thread_pool_(thread_pool_size),
      scheduler_(scheduler != nullptr ? std::move(scheduler)
                                      : std::make_unique<AnyStdExecScheduler<WorkSharingThreadPool::Scheduler>>(
                                            default_work_sharing_thread_pool_.GetScheduler())),
      broker_actor_(cluster_config.this_node.address.empty()
                        ? nullptr
                        : std::make_unique<Actor<MessageBroker>>(scheduler_->Clone(), ActorConfig {}, cluster_config)),
      broker_actor_ref_(broker_actor_ ? LocalActorRef<MessageBroker>(/*actor_id=*/UINT64_MAX, broker_actor_.get())
                                      : LocalActorRef<MessageBroker> {}),
      backend_actor_(scheduler_->Clone(), ActorConfig {}, scheduler_->Clone(), cluster_config, broker_actor_ref_),
      backend_actor_ref_(/*actor_id=*/UINT64_MAX, &backend_actor_) {
  if (!broker_actor_ref_.IsEmpty()) {
    NotifyOnSpawned<MessageBroker>(broker_actor_.get(), broker_actor_ref_);
    MessageBroker::RequestHandler request_handler =
        [ref = backend_actor_ref_](ByteBuffer data) -> exec::task<ByteBuffer> {
      co_return co_await ref.SendLocal<&ActorRegistryBackend::HandleNetworkRequest>(std::move(data));
    };
    ex::sync_wait(broker_actor_ref_.SendLocal<&MessageBroker::Start>(std::move(request_handler)));
  }
  // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
}

exec::task<bool> ActorRegistry::WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  if (node_id == this_node_id_) {
    co_return true;
  }
  co_return co_await backend_actor_ref_.SendLocal<&ActorRegistryBackend::WaitNodeAlive>(node_id, timeout_ms);
}

}  // namespace ex_actor::internal
