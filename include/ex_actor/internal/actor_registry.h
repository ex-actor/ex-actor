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
#include "ex_actor/internal/constants.h"
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
  explicit ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                const ClusterConfig& cluster_config, MessageBroker* message_broker);

  exec::task<void> AsyncDestroyAllActors();

  /**
   * @brief Spawn a local actor with config.
   */
  template <class UserClass, class... Args>
  exec::task<ActorRef<UserClass>> SpawnWithClass(ActorConfig config, Args... args) {
    if (config.node_id != this_node_id_) {
      EXA_THROW << "Spawn<UserClass> can only be used to create local actor, to create remote actor, use "
                   "Spawn<&CreateFn> to provide a fixed signature for remote actor creation. node_id="
                << config.node_id << ", this_node_id=" << this_node_id_ << ", actor_type=" << typeid(UserClass).name();
    }
    co_return co_await SpawnLocal<UserClass>(std::move(config), std::move(args)...);
  }

  /**
   * @brief Spawn an actor (local or remote) using a factory function, with config.
   */
  template <auto kCreateFn, class... Args>
  exec::task<ActorRef<FnReturnType<kCreateFn>>> SpawnWithCreateFn(ActorConfig config, Args... args) {
    using UserClass = FnReturnType<kCreateFn>;
    static_assert(std::is_invocable_v<decltype(kCreateFn), Args...>,
                  "Class can't be created by given args and create function");

    if (config.node_id == this_node_id_) {
      co_return co_await SpawnLocal<UserClass, kCreateFn>(std::move(config), std::move(args)...);
    }

    if (message_broker_ != nullptr) {
      EXA_THROW_CHECK(message_broker_->CheckNodeConnected(config.node_id)) << "Can't find node " << config.node_id;
    }

    using CreateFnSig = Signature<decltype(kCreateFn)>;

    typename CreateFnSig::DecayedArgsTupleType args_tuple {std::move(args)...};
    ActorCreationArgs<typename CreateFnSig::DecayedArgsTupleType> actor_creation_args {config, std::move(args_tuple)};

    NetworkRequest request {ActorCreationRequest {
        .handler_key = GetUniqueNameForFunction<kCreateFn>(),
        .serialized_args = Serialize(actor_creation_args),
    }};

    ByteBuffer response_buf = co_await message_broker_->SendRequest(config.node_id, Serialize(request));
    auto reply = Deserialize<NetworkReply>(response_buf);

    auto& ret = std::get<ActorCreationReply>(reply.variant);
    if (!ret.success) {
      EXA_THROW << "Got actor creation error from remote node:" << ret.error;
    }
    co_return ActorRef<UserClass>(this_node_id_, config.node_id, ret.actor_id, nullptr, message_broker_);
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

    NetworkRequest request {ActorLookUpRequest {.actor_name = name}};
    ByteBuffer response_buf = co_await message_broker_->SendRequest(node_id, Serialize(request));
    auto reply = Deserialize<NetworkReply>(response_buf);

    auto& ret = std::get<ActorLookUpReply>(reply.variant);
    if (ret.success) {
      co_return ActorRef<UserClass>(this_node_id_, node_id, ret.actor_id, nullptr, message_broker_);
    }
    co_return std::nullopt;
  }

  exec::task<void> HandleNetworkRequest(uint64_t received_request_id, ByteBuffer request_buffer);
  exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms);

 private:
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  exec::task<ActorRef<UserClass>> SpawnLocal(ActorConfig config, Args... args) {
    auto actor_id = GenerateRandomActorId();
    auto actor = std::make_unique<Actor<UserClass, kCreateFn>>(scheduler_->Clone(), config, std::move(args)...);
    auto handle = ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, actor.get(), message_broker_);
    if (config.actor_name.has_value()) {
      std::string& name = *config.actor_name;
      EXA_THROW_CHECK(!actor_name_to_id_.contains(name)) << "An actor with the same name already exists, name=" << name;
      actor_name_to_id_[name] = actor_id;
    }
    actor_id_to_actor_[actor_id] = std::move(actor);
    co_return handle;
  }

  std::mt19937 random_num_generator_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  uint32_t this_node_id_ = 0;
  MessageBroker* message_broker_ = nullptr;
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unordered_map<std::string, std::uint64_t> actor_name_to_id_;

  void InitRandomNumGenerator();
  uint64_t GenerateRandomActorId();
  void ReplyToBroker(uint64_t received_request_id, const NetworkReply& reply);
  void HandleActorCreationRequest(uint64_t received_request_id, ActorCreationRequest msg);
  exec::task<void> HandleActorMethodCallRequest(uint64_t received_request_id, ActorMethodCallRequest msg);
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
      : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr, /*cluster_config=*/ {}) {}

  /**
   * @brief Construct in single-node mode, use specified scheduler.
   */
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler)
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler),
                      /*cluster_config*/ {}) {}

  /**
   * @brief Construct in distributed mode, use the default work-sharing thread pool as the scheduler.
   */
  explicit ActorRegistry(uint32_t thread_pool_size, const ClusterConfig& cluster_config)
      : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr, cluster_config) {}

  /**
   * @brief Construct in distributed mode, use specified scheduler.
   */
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler, const ClusterConfig& cluster_config)
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler),
                      cluster_config) {}

  ~ActorRegistry();

  /**
   * @brief Spawn a local actor at current node using default config.
   */
  template <class UserClass, class... Args>
  AwaitableOf<ActorRef<UserClass>> auto Spawn(Args... args) {
    return WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::SpawnWithClass<UserClass, Args...>>(
            ActorConfig {.node_id = this_node_id_}, std::move(args)...));
  }

  /**
   * @brief Spawn a local actor with a manually specified config.
   */
  template <class UserClass, class... Args>
  AwaitableOf<ActorRef<UserClass>> auto Spawn(ActorConfig config, Args... args) {
    return WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::SpawnWithClass<UserClass, Args...>>(config,
                                                                                                std::move(args)...));
  }

  /**
   * @brief Spawn an actor (local or remote) using a factory function, at current node using default config.
   */
  template <auto kCreateFn, class... Args>
  AwaitableOf<ActorRef<FnReturnType<kCreateFn>>> auto Spawn(Args... args) {
    return WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::SpawnWithCreateFn<kCreateFn, Args...>>(
            ActorConfig {.node_id = this_node_id_}, std::move(args)...));
  }

  /**
   * @brief Spawn an actor (local or remote) using a factory function, with a manually specified config.
   */
  template <auto kCreateFn, class... Args>
  AwaitableOf<ActorRef<FnReturnType<kCreateFn>>> auto Spawn(ActorConfig config, Args... args) {
    return WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::SpawnWithCreateFn<kCreateFn, Args...>>(config,
                                                                                                   std::move(args)...));
  }

  template <class UserClass>
  AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
    return WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::DestroyActor<UserClass>>(actor_ref));
  }

  /**
   * @brief Find the actor by name at current node.
   */
  template <class UserClass>
  AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) const {
    // resolve overload ambiguity
    constexpr std::optional<ActorRef<UserClass>> (ActorRegistryBackend::*kProcessFn)(const std::string& name) const =
        &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(name));
  }

  /**
   * @brief Find the actor by name at remote node.
   */
  template <class UserClass>
  AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                         const std::string& name) const {
    // resolve overload ambiguity
    constexpr exec::task<std::optional<ActorRef<UserClass>>> (ActorRegistryBackend::*kProcessFn)(
        const uint32_t& node_id, const std::string& name) const = &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(node_id, name));
  }

  exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms);

  // ------------deprecated APIs------------

  /// Backward-compatible alias for Spawn.
  template <class UserClass, class... Args>
  [[deprecated("Use Spawn() instead of CreateActor().")]]
  AwaitableOf<ActorRef<UserClass>> auto CreateActor(Args... args) {
    return Spawn<UserClass, Args...>(std::move(args)...);
  }
  /// Backward-compatible alias for Spawn.
  template <class UserClass, class... Args>
  [[deprecated("Use Spawn() instead of CreateActor().")]]
  AwaitableOf<ActorRef<UserClass>> auto CreateActor(ActorConfig config, Args... args) {
    return Spawn<UserClass, Args...>(std::move(config), std::move(args)...);
  }

 private:
  uint32_t this_node_id_;
  WorkSharingThreadPool default_work_sharing_thread_pool_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  std::unique_ptr<MessageBroker> message_broker_;
  Actor<ActorRegistryBackend> backend_actor_;
  ActorRef<ActorRegistryBackend> backend_actor_ref_;
  exec::async_scope async_scope_;

  explicit ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                         const ClusterConfig& cluster_config);
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

void Init(uint32_t thread_pool_size, const ClusterConfig& cluster_config);

template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, const ClusterConfig& cluster_config);

exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms);

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
 * @brief Spawn a local actor at current node using default config.
 */
template <class UserClass, class... Args>
internal::AwaitableOf<ActorRef<UserClass>> auto Spawn(Args... args) {
  return internal::GetGlobalDefaultRegistry().Spawn<UserClass, Args...>(std::move(args)...);
}

/**
 * @brief Spawn a local actor with a manually specified config.
 */
template <class UserClass, class... Args>
internal::AwaitableOf<ActorRef<UserClass>> auto Spawn(ActorConfig config, Args... args) {
  return internal::GetGlobalDefaultRegistry().Spawn<UserClass, Args...>(config, std::move(args)...);
}

/**
 * @brief Spawn an actor (local or remote) using a factory function, at current node using default config.
 */
template <auto kCreateFn, class... Args>
internal::AwaitableOf<ActorRef<internal::FnReturnType<kCreateFn>>> auto Spawn(Args... args) {
  return internal::GetGlobalDefaultRegistry().Spawn<kCreateFn, Args...>(std::move(args)...);
}

/**
 * @brief Spawn an actor (local or remote) using a factory function, with a manually specified config.
 */
template <auto kCreateFn, class... Args>
internal::AwaitableOf<ActorRef<internal::FnReturnType<kCreateFn>>> auto Spawn(ActorConfig config, Args... args) {
  return internal::GetGlobalDefaultRegistry().Spawn<kCreateFn, Args...>(config, std::move(args)...);
}

/**
 * @brief Destroy an actor.
 */
template <class UserClass>
internal::AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
  return internal::GetGlobalDefaultRegistry().DestroyActor<UserClass>(actor_ref);
}

/**
 * @brief Find the actor by name at current node.
 */
template <class UserClass>
internal::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(name);
}

/**
 * @brief Find the actor by name at specified node.
 */
template <class UserClass>
internal::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                                 const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(node_id, name);
}

/**
 * @brief Configure the logging of ex_actor. Not thread-safe, please call it only when no logs are printing.
 */
void ConfigureLogging(const LogConfig& config = {});

// -------------deprecated APIs-------------

/// Init the global default registry in distributed mode, use specified scheduler. Not thread-safe.
template <ex::scheduler Scheduler>
[[deprecated(
    "Deprecated: use cluster_config-based initialization: `Init(uint32_t, const ClusterConfig&)` "
    "or `ActorRegistry(Scheduler, const ClusterConfig&)`. This API will be removed in the future.")]]
void Init(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info);
/// Init the global default registry in distributed mode, use the default work-sharing thread pool as the scheduler. Not
/// thread-safe.
[[deprecated(
    "Deprecated: use `Init(uint32_t, const ClusterConfig&)` instead. This API will be removed in the future.")]]
void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info);

}  // namespace ex_actor

// -----------template function implementations-------------

namespace ex_actor {
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler) {
  internal::log::Info("Initializing ex_actor in single-node mode with custom scheduler.");
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler)));
  internal::SetupGlobalHandlers();
}

template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::log::Info(
      "Initializing ex_actor in distributed mode with custom scheduler. this_node_id={}, total_nodes={}", this_node_id,
      cluster_node_info.size());
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  ClusterConfig cluster_config {};
  NodeInfo this_node {.node_id = this_node_id};
  NodeInfo contact_node {.node_id = this_node_id};
  for (const auto& node : cluster_node_info) {
    if (node.node_id < contact_node.node_id) {
      contact_node.node_id = node.node_id;
      contact_node.address = node.address;
    }

    if (node.node_id == this_node_id) {
      this_node.address = node.address;
    }
  }
  cluster_config.this_node = this_node;
  cluster_config.contact_node = contact_node;

  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler), cluster_config));
  internal::SetupGlobalHandlers();
  for (const auto& node : cluster_node_info) {
    if (node.node_id != this_node_id) {
      auto [connected] =
          stdexec::sync_wait(WaitNodeAlive(node.node_id, internal::kDefaultWaitNodeAliveTimeoutMs)).value();
      EXA_THROW_CHECK(connected) << "Cannot connect to the node " << node.node_id;
    }
  }
}

template <ex::scheduler Scheduler>
void Init(Scheduler scheduler, const ClusterConfig& cluster_config) {
  internal::log::Info("Initializing ex_actor in distributed mode with custom scheduler. this_node_id={}",
                      cluster_config.this_node.node_id);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler), cluster_config));
  internal::SetupGlobalHandlers();
}

}  // namespace ex_actor
