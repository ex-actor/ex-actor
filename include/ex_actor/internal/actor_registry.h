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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/actor_ref.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/scheduler.h"
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {

/**
 * @brief An actor wrapper of the most functions of ActorRegistry. Bootstraps itself as an actor, so it can reuse the
 * core functionality of Actors like mailboxes, schedulers, etc.
 *
 */
class ActorRegistryBackend {
 public:
  explicit ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler, uint32_t this_node_id,
                                const std::vector<NodeInfo>& cluster_node_info, network::MessageBroker* message_broker);

  exec::task<void> AsyncDestroyAllActors();

  /**
   * @brief Create actor at current node using default config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  exec::task<ActorRef<UserClass>> CreateActor(Args... args) {
    return CreateActor<UserClass, kCreateFn>(ActorConfig {.node_id = this_node_id_}, std::move(args)...);
  }

  /**
   * @brief Create an actor with a manually specified config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  exec::task<ActorRef<UserClass>> CreateActor(ActorConfig config, Args... args) {
    if constexpr (kCreateFn != nullptr) {
      static_assert(std::is_invocable_v<decltype(kCreateFn), Args...>,
                    "Class can't be created by given args and create function");
    }
    if (is_distributed_mode_) {
      EXA_THROW_CHECK(node_id_to_address_.contains(config.node_id)) << "Invalid node id: " << config.node_id;
    }

    // local actor, create directly
    if (config.node_id == this_node_id_) {
      auto actor_id = GenerateRandomActorId();
      auto actor = std::make_unique<Actor<UserClass, kCreateFn>>(scheduler_->Clone(), config, std::move(args)...);
      auto handle = ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, actor.get(), message_broker_);
      if (config.actor_name.has_value()) {
        std::string& name = *config.actor_name;
        EXA_THROW_CHECK(!actor_name_to_id_.contains(name))
            << "An actor with the same name already exists, name=" << name;
        actor_name_to_id_[name] = actor_id;
      }
      actor_id_to_actor_[actor_id] = std::move(actor);
      co_return handle;
    }

    if constexpr (kCreateFn == nullptr) {
      EXA_THROW << "CreateActor<UserClass> can only be used to create local actor, to create remote actor, use "
                   "CreateActor<UserClass, kCreateFn> to provide a fixed signature for remote actor creation. node_id="
                << config.node_id << ", this_node_id=" << this_node_id_ << ", actor_type=" << typeid(UserClass).name();
    }

    if constexpr (kCreateFn != nullptr) {
      using CreateFnSig = reflect::Signature<decltype(kCreateFn)>;

      // protocol: [message_type][handler_key_len][handler_key][ActorCreationArgs]
      typename CreateFnSig::DecayedArgsTupleType args_tuple {std::move(args)...};
      serde::ActorCreationArgs<typename CreateFnSig::DecayedArgsTupleType> actor_creation_args {config,
                                                                                                std::move(args_tuple)};
      std::vector<char> serialized = serde::Serialize(actor_creation_args);
      std::string handler_key = reflect::GetUniqueNameForFunction<kCreateFn>();
      serde::BufferWriter buffer_writer(network::ByteBufferType {
          serialized.size() + sizeof(uint64_t) + handler_key.size() + sizeof(serde::NetworkRequestType)});
      buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorCreationRequest);
      buffer_writer.WritePrimitive(handler_key.size());
      // TODO optimize the copy here
      buffer_writer.CopyFrom(handler_key.data(), handler_key.size());
      buffer_writer.CopyFrom(serialized.data(), serialized.size());
      EXA_THROW_CHECK(buffer_writer.EndReached()) << "Buffer writer not ended";

      // send to remote
      auto response_buffer =
          co_await message_broker_->SendRequest(config.node_id, std::move(buffer_writer).MoveBufferOut());
      serde::BufferReader reader(std::move(response_buffer));
      auto type = reader.template NextPrimitive<serde::NetworkReplyType>();
      if (type == serde::NetworkReplyType::kActorCreationError) {
        EXA_THROW << "Got actor creation error from remote node:"
                  << serde::Deserialize<serde::ActorCreationError>(reader.Current(), reader.RemainingSize()).error;
      }
      auto actor_id = reader.template NextPrimitive<uint64_t>();
      co_return ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, nullptr, message_broker_);
    }
  }

  template <class UserClass>
  exec::task<void> DestroyActor(const ActorRef<UserClass>& actor_ref) {
    auto actor_id = actor_ref.GetActorId();
    EXA_THROW_CHECK(actor_id_to_actor_.contains(actor_id)) << "Actor with id " << actor_id << " not found";
    auto actor = std::move(actor_id_to_actor_.at(actor_id));
    actor_id_to_actor_.erase(actor_id);

    auto actor_name = actor->GetActorConfig().actor_name;
    if (actor_name.has_value()) {
      actor_name_to_id_.erase(actor_name.value());
    }
    co_await actor->AsyncDestroy();
  }

  template <class UserClass>
  std::optional<ActorRef<UserClass>> GetActorRefByName(const std::string& name) const {
    if (actor_name_to_id_.contains(name)) {
      const auto actor_id = actor_name_to_id_.at(name);
      const auto& actor = actor_id_to_actor_.at(actor_id);
      return ActorRef<UserClass>(this_node_id_, this_node_id_, actor_id, actor.get(), message_broker_);
    }
    return std::nullopt;
  }

  template <class UserClass>
  exec::task<std::optional<ActorRef<UserClass>>> GetActorRefByName(const uint32_t& node_id,
                                                                   const std::string& name) const {
    if (node_id == this_node_id_) {
      co_return GetActorRefByName<UserClass>(name);
    }

    std::vector<char> serialized = serde::Serialize(serde::ActorLookUpRequest {name});
    serde::BufferWriter<network::ByteBufferType> writer(
        network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
    writer.WritePrimitive(serde::NetworkRequestType::kActorLookUpRequest);
    writer.CopyFrom(serialized.data(), serialized.size());

    auto response_buffer =
        co_await message_broker_->SendRequest(node_id, network::ByteBufferType {std::move(writer).MoveBufferOut()});

    std::optional<uint64_t> actor_id = std::nullopt;
    serde::BufferReader<network::ByteBufferType> reader(std::move(response_buffer));
    auto type = reader.NextPrimitive<serde::NetworkReplyType>();
    if (type == serde::NetworkReplyType::kActorLookUpReturn) {
      actor_id = reader.NextPrimitive<uint64_t>();
    }

    if (actor_id.has_value()) {
      co_return ActorRef<UserClass>(this_node_id_, node_id, actor_id.value(), nullptr, message_broker_);
    }
    co_return std::nullopt;
  }

  exec::task<void> HandleNetworkRequest(uint64_t received_request_id, network::ByteBufferType request_buffer);
  bool CheckNode(uint32_t node_id);

 private:
  bool is_distributed_mode_ = false;
  std::mt19937 random_num_generator_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  uint32_t this_node_id_ = 0;
  std::unordered_map<uint32_t, std::string> node_id_to_address_;
  network::MessageBroker* message_broker_ = nullptr;
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unordered_map<std::string, std::uint64_t> actor_name_to_id_;

  void InitRandomNumGenerator();
  uint64_t GenerateRandomActorId();
  void ValidateNodeInfo(const std::vector<NodeInfo>& cluster_node_info);
  serde::NetworkRequestType ParseMessageType(const network::ByteBufferType& buffer);
  void ReplyError(uint64_t received_request_id, serde::NetworkReplyType reply_type, std::string error_msg);
  void HandleActorCreationRequest(uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader);
  exec::task<void> HandleActorMethodCallRequest(uint64_t received_request_id,
                                                serde::BufferReader<network::ByteBufferType> reader);
};

/**
 * @brief The public API of ActorRegistry. User can use this class to create and manage actors.
 * It simply forwards everything to the backend actor.
 */
class ActorRegistry {
 public:
  /**
   * @brief Construct in single-node mode, use the default work-sharing thread pool as the scheduler.
   */
  explicit ActorRegistry(uint32_t thread_pool_size)
      : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr, /*this_node_id=*/0, /*cluster_node_info=*/ {},
                      /*heartbeat_config=*/ {}) {}

  /**
   * @brief Construct in single-node mode, use specified scheduler.
   */
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler)
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler),
                      /*this_node_id=*/0, /*cluster_node_info=*/ {}, /*heartbeat_config=*/ {}) {}

  /**
   * @brief Construct in distributed mode, use the default work-sharing thread pool as the scheduler.
   */
  explicit ActorRegistry(uint32_t thread_pool_size, uint32_t this_node_id,
                         const std::vector<NodeInfo>& cluster_node_info, network::HeartbeatConfig heartbeat_config = {})
      : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr, this_node_id, cluster_node_info, heartbeat_config) {}

  /**
   * @brief Construct in distributed mode, use specified scheduler.
   */
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config = {})
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler), this_node_id,
                      cluster_node_info, heartbeat_config) {}

  ~ActorRegistry();

  /**
   * @brief Create actor at current node using default config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  reflect::AwaitableOf<ActorRef<UserClass>> auto CreateActor(Args... args) {
    // resolve overload ambiguity
    constexpr exec::task<ActorRef<UserClass>> (ActorRegistryBackend::*kProcessFn)(Args...) =
        &ActorRegistryBackend::CreateActor<UserClass, kCreateFn, Args...>;

    return util::WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(std::move(args)...));
  }

  /**
   * @brief Create an actor with a manually specified config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  reflect::AwaitableOf<ActorRef<UserClass>> auto CreateActor(ActorConfig config, Args... args) {
    // resolve overload ambiguity
    constexpr exec::task<ActorRef<UserClass>> (ActorRegistryBackend::*kProcessFn)(ActorConfig, Args...) =
        &ActorRegistryBackend::CreateActor<UserClass, kCreateFn, Args...>;

    return util::WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(config, std::move(args)...));
  }

  template <class UserClass>
  reflect::AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
    return util::WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::DestroyActor<UserClass>>(actor_ref));
  }

  /**
   * @brief Find the actor by name at current node.
   */
  template <class UserClass>
  reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) const {
    // resolve overload ambiguity
    constexpr std::optional<ActorRef<UserClass>> (ActorRegistryBackend::*kProcessFn)(const std::string& name) const =
        &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return util::WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(name));
  }

  /**
   * @brief Find the actor by name at remote node.
   */
  template <class UserClass>
  reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                                  const std::string& name) const {
    // resolve overload ambiguity
    constexpr exec::task<std::optional<ActorRef<UserClass>>> (ActorRegistryBackend::*kProcessFn)(
        const uint32_t& node_id, const std::string& name) const = &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return util::WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(node_id, name));
  }

  bool WaitNode(uint32_t node_id);

 private:
  bool is_distributed_mode_;
  uint32_t this_node_id_;
  WorkSharingThreadPool default_work_sharing_thread_pool_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  std::unique_ptr<network::MessageBroker> message_broker_;
  Actor<ActorRegistryBackend> backend_actor_;
  ActorRef<ActorRegistryBackend> backend_actor_ref_;
  exec::async_scope async_scope_;

  explicit ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                         uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config = {},
                         std::chrono::milliseconds gossip_interval = kDefaultGossipInterval);
};

}  // namespace ex_actor::internal

// ----------------Global Default Registry--------------------------

namespace ex_actor::internal {
ActorRegistry& GetGlobalDefaultRegistry();
void AssignGlobalDefaultRegistry(std::unique_ptr<ActorRegistry> registry);
bool IsGlobalDefaultRegistryInitialized();
void SetupGlobalHandlers();
}  // namespace ex_actor::internal

namespace ex_actor {
using ex_actor::internal::ActorRegistry;

/**
 * @brief Init the global default registry in single-node mode, use the default work-sharing thread pool as the
 * scheduler. Not thread-safe.
 */
void Init(uint32_t thread_pool_size);

/**
 * @brief Init the global default registry in single-node mode, use specified scheduler. Not thread-safe.
 */
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler);

/**
 * @brief Init the global default registry in distributed mode, use the default work-sharing thread pool as the
 * scheduler. Not thread-safe.
 */
void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info);

/**
 * @brief Init the global default registry in distributed mode, use specified scheduler. Not thread-safe.
 */
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info);

void Init(uint32_t thread_pool_size, const ClusterConfig& cluster_config);

bool WaitNode(uint32_t node_id, std::chrono::milliseconds timeout);
/**
 * @brief Ask ex_actor to hold a resource, the resource won't be released until runtime is shut down.
 * Often used to hold an execution resource when using custom scheduler. This API is not thread-safe.
 * @param resource The resource to hold. Can pass any `unique_ptr` or `shared_ptr`.
 */
void HoldResource(std::shared_ptr<void> resource);

/**
 * @brief Shutdown the global default registry. Must be called explicitly before `main` exits. Not thread-safe.
 */
void Shutdown();

/**
 * @brief Create actor at current node using default config.
 */
template <class UserClass, auto kCreateFn = nullptr, class... Args>
internal::reflect::AwaitableOf<ActorRef<UserClass>> auto Spawn(Args... args) {
  return internal::GetGlobalDefaultRegistry().CreateActor<UserClass, kCreateFn, Args...>(std::move(args)...);
}

/**
 * @brief Create an actor with a manually specified config.
 */
template <class UserClass, auto kCreateFn = nullptr, class... Args>
internal::reflect::AwaitableOf<ActorRef<UserClass>> auto Spawn(ActorConfig config, Args... args) {
  return internal::GetGlobalDefaultRegistry().CreateActor<UserClass, kCreateFn, Args...>(config, std::move(args)...);
}

/**
 * @brief Destroy an actor.
 */
template <class UserClass>
internal::reflect::AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
  return internal::GetGlobalDefaultRegistry().DestroyActor<UserClass>(actor_ref);
}

/**
 * @brief Find the actor by name at current node.
 */
template <class UserClass>
internal::reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(name);
}

/**
 * @brief Find the actor by name at specified node.
 */
template <class UserClass>
internal::reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                                          const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(node_id, name);
}

/**
 * @brief Configure the logging of ex_actor. Not thread-safe, please call it only when no logs are printing.
 */
void ConfigureLogging(const logging::LogConfig& config = {});
}  // namespace ex_actor

// -----------template function implementations-------------

namespace ex_actor {
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler) {
  internal::logging::Info("Initializing ex_actor in single-node mode with custom scheduler.");
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler)));
  internal::SetupGlobalHandlers();
}

template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::logging::Info(
      "Initializing ex_actor in distributed mode with custom scheduler. this_node_id={}, total_nodes={}", this_node_id,
      cluster_node_info.size());
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler), this_node_id, cluster_node_info));
  internal::SetupGlobalHandlers();
}
}  // namespace ex_actor
