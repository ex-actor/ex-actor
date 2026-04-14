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
#include <tuple>
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

namespace ex_actor::internal {
/**
 * @brief An actor wrapper of the most functions of ActorRegistry. Bootstraps itself as an actor, so it can reuse the
 * core functionality of Actors like mailboxes, schedulers, etc.
 *
 */
class ActorRegistryBackend {
 public:
  explicit ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler, uint64_t this_node_id,
                                BasicActorRef<MessageBroker> broker_actor_ref);

  ex::task<void> AsyncDestroyAllActors();

  /**
   * @brief Spawn a local actor with config.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> SpawnWithClass(uint64_t node_id, ActorConfig config, Args... args) {
    if (node_id != this_node_id_) {
      EXA_THROW << "Spawn<UserClass> can only be used to create local actor, to create remote actor, use "
                   "Spawn<&CreateFn> to provide a fixed signature for remote actor creation. node_id="
                << node_id << ", this_node_id=" << this_node_id_ << ", actor_type=" << typeid(UserClass).name();
    }
    return SpawnLocal<UserClass>(node_id, std::move(config), std::move(args)...);
  }

  /**
   * @brief Spawn an actor (local or remote) using a factory function, with config.
   */
  template <auto kCreateFn, class... Args>
  ex::task<ActorRef<FnReturnType<kCreateFn>>> SpawnWithCreateFn(uint64_t node_id, ActorConfig config, Args... args) {
    using UserClass = FnReturnType<kCreateFn>;
    if (node_id == this_node_id_) {
      co_return SpawnLocal<UserClass, kCreateFn>(node_id, std::move(config), std::move(args)...);
    }

    EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty()) << "Broker actor not set";

    using CreateFnSig = Signature<decltype(kCreateFn)>;

    typename CreateFnSig::DecayedArgsTupleType args_tuple {std::move(args)...};
    ActorCreationArgs<typename CreateFnSig::DecayedArgsTupleType> actor_creation_args {config, std::move(args_tuple)};

    NetworkRequest request {ActorCreationRequest {
        .handler_key = GetUniqueNameForFunction<kCreateFn>(),
        .serialized_args = Serialize(actor_creation_args),
    }};

    ByteBuffer response_buf =
        co_await broker_actor_ref_.SendLocal<&MessageBroker::SendRequest>(node_id, Serialize(request));
    auto reply = Deserialize<NetworkReply>(response_buf);

    auto& ret = std::get<ActorCreationReply>(reply.variant);
    if (!ret.success) {
      EXA_THROW << "Got actor creation error from remote node:" << ret.error;
    }
    co_return ActorRef<UserClass>(this_node_id_, node_id, ret.actor_id, /*actor=*/nullptr, broker_actor_ref_);
  }

  template <class UserClass>
  ex::task<void> DestroyActor(const ActorRef<UserClass>& actor_ref) {
    auto actor_id = actor_ref.GetActorId();
    auto node_id = actor_ref.GetNodeId();

    if (node_id == this_node_id_) {
      co_await DestroyLocalActor(actor_id);
      co_return;
    }

    EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty()) << "Trying to destroy a remote actor, but not in distributed mode";

    NetworkRequest request {ActorDestroyRequest {.actor_id = actor_id}};
    ByteBuffer response_buf =
        co_await broker_actor_ref_.SendLocal<&MessageBroker::SendRequest>(node_id, Serialize(request));
    auto reply = Deserialize<NetworkReply>(response_buf);

    auto& ret = std::get<ActorDestroyReply>(reply.variant);
    if (!ret.success) {
      EXA_THROW << "Got actor destroy error from remote node: " << ret.error;
    }
  }

  template <class UserClass>
  std::optional<ActorRef<UserClass>> GetActorRefByName(const std::string& name) const {
    if (actor_name_to_id_.contains(name)) {
      const auto actor_id = actor_name_to_id_.at(name);
      const auto& actor = actor_id_to_actor_.at(actor_id);
      return ActorRef<UserClass>(this_node_id_, this_node_id_, actor_id, actor.get(), broker_actor_ref_);
    }
    return std::nullopt;
  }

  template <class UserClass>
  ex::task<std::optional<ActorRef<UserClass>>> GetActorRefByName(const uint64_t& node_id,
                                                                 const std::string& name) const {
    if (node_id == this_node_id_) {
      co_return GetActorRefByName<UserClass>(name);
    }

    EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty()) << "Broker actor not set";

    NetworkRequest request {ActorLookUpRequest {.actor_name = name}};
    ByteBuffer response_buf =
        co_await broker_actor_ref_.SendLocal<&MessageBroker::SendRequest>(node_id, Serialize(request));
    auto reply = Deserialize<NetworkReply>(response_buf);

    auto& ret = std::get<ActorLookUpReply>(reply.variant);
    if (ret.success) {
      co_return ActorRef<UserClass>(this_node_id_, node_id, ret.actor_id, /*actor=*/nullptr, broker_actor_ref_);
    }
    co_return std::nullopt;
  }

  /**
   * @brief Update the broker actor ref. Called when switching to distributed mode after init.
   */
  void SetBrokerActorRef(BasicActorRef<MessageBroker> broker_actor_ref);

  /**
   * @brief Handle a network request. Returns the serialized reply data.
   */
  ex::task<ByteBuffer> HandleNetworkRequest(ByteBuffer request_buffer);

  ex::task<void> DestroyLocalActor(uint64_t actor_id);

 private:
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  ActorRef<UserClass> SpawnLocal(uint64_t node_id, ActorConfig config, Args... args) {
    auto actor_id = GenerateRandomActorId();
    auto actor = std::make_unique<Actor<UserClass, kCreateFn>>(scheduler_->Clone(), config, std::move(args)...);
    auto handle = ActorRef<UserClass>(this_node_id_, node_id, actor_id, actor.get(), broker_actor_ref_);
    NotifyOnSpawned<UserClass>(actor.get(), handle);
    if (config.actor_name.has_value()) {
      std::string& name = *config.actor_name;
      EXA_THROW_CHECK(!actor_name_to_id_.contains(name)) << "An actor with the same name already exists, name=" << name;
      actor_name_to_id_[name] = actor_id;
    }
    actor_id_to_actor_[actor_id] = std::move(actor);
    return handle;
  }

  std::mt19937 random_num_generator_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  uint64_t this_node_id_ = 0;
  BasicActorRef<MessageBroker> broker_actor_ref_;
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unordered_map<std::string, std::uint64_t> actor_name_to_id_;

  void InitRandomNumGenerator();
  uint64_t GenerateRandomActorId();
  ByteBuffer SerializeReply(const NetworkReply& reply);
  void HandleActorCreationRequest(ActorCreationRequest msg, ByteBuffer& reply_out);
  ex::task<ByteBuffer> HandleActorMethodCallRequest(ActorMethodCallRequest msg);
  ex::task<ByteBuffer> HandleActorDestroyRequest(ActorDestroyRequest msg);
};

// -----------SpawnBuilder: a builder-style sender for actor spawning-----------

/**
 * @brief A builder-style sender returned by Spawn(). It is itself a sender (satisfying the std::execution sender
 * concept), so it can be directly co_await-ed or composed with other senders. Call .ToNode() to target a specific
 * node, and .WithConfig() to attach an ActorConfig before awaiting.
 *
 * @code
 * // Default (local node, default config):
 * auto ref = co_await ex_actor::Spawn<Foo>(p1, p2);
 *
 * // Spawn on a remote node:
 * auto ref = co_await ex_actor::Spawn<&Foo::Create>(p1, p2).ToNode(remote_node_id);
 *
 * // Spawn on a remote node with extra config:
 * auto ref = co_await ex_actor::Spawn<&Foo::Create>(p1).ToNode(remote_node_id).WithConfig({.actor_name = "foo"});
 * @endcode
 *
 * @tparam UserClass The actor class to spawn.
 * @tparam kCreateFn Factory function pointer, or nullptr for direct construction.
 * @tparam Args Constructor / factory argument types.
 */
template <class UserClass, auto kCreateFn, class... Args>
class SpawnBuilder : public ex::sender_t {
 public:
  using completion_signatures = ex::completion_signatures<ex::set_value_t(ActorRef<UserClass>),
                                                          ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;

  explicit SpawnBuilder(BasicActorRef<ActorRegistryBackend> backend_ref, Args... args)
      : backend_ref_(std::move(backend_ref)), args_(std::move(args)...) {}

  SpawnBuilder& ToNode(uint64_t node_id) & {
    node_id_ = node_id;
    return *this;
  }

  SpawnBuilder&& ToNode(uint64_t node_id) && {
    node_id_ = node_id;
    return std::move(*this);
  }

  SpawnBuilder& WithConfig(ActorConfig config) & {
    config_ = std::move(config);
    return *this;
  }

  SpawnBuilder&& WithConfig(ActorConfig config) && {
    config_ = std::move(config);
    return std::move(*this);
  }

  template <ex::receiver Receiver>
    requires(std::copyable<Args> && ...)
  auto connect(Receiver receiver) & {
    return ex::connect(MakeSender(node_id_, config_, args_), std::move(receiver));
  }

  template <ex::receiver Receiver>
  auto connect(Receiver receiver) && {
    return ex::connect(MakeSender(node_id_, std::move(config_), std::move(args_)), std::move(receiver));
  }

 private:
  template <class Config, class Tuple>
  auto MakeSender(uint64_t node_id, Config&& config, Tuple&& args) {
    return std::apply(
        [this, node_id, config = std::forward<Config>(config)](auto&&... a) mutable {
          if constexpr (kCreateFn == nullptr) {
            return backend_ref_.template SendLocal<&ActorRegistryBackend::SpawnWithClass<UserClass, Args...>>(
                node_id, std::move(config), std::forward<decltype(a)>(a)...);
          } else {
            return backend_ref_.template SendLocal<&ActorRegistryBackend::SpawnWithCreateFn<kCreateFn, Args...>>(
                node_id, std::move(config), std::forward<decltype(a)>(a)...);
          }
        },
        std::forward<Tuple>(args));
  }

  BasicActorRef<ActorRegistryBackend> backend_ref_;
  uint64_t node_id_ = 0;
  ActorConfig config_ {};
  std::tuple<Args...> args_;
};

/**
 * @brief The public API of ActorRegistry. User can use this class to create and manage actors.
 * It simply forwards everything to the backend actor.
 */
class ActorRegistry {
 public:
  /**
   * @brief Construct with the default work-sharing thread pool as the scheduler.
   */
  explicit ActorRegistry(uint32_t thread_pool_size) : ActorRegistry(thread_pool_size, /*scheduler=*/nullptr) {}

  /**
   * @brief Construct with a user-specified scheduler.
   */
  template <ex::scheduler Scheduler>
  explicit ActorRegistry(Scheduler scheduler)
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler)) {}

  ~ActorRegistry();

  /** @copydoc ex_actor::Spawn */
  template <class UserClass, class... Args>
  SenderOf<ActorRef<UserClass>> auto Spawn(Args... args)
    requires(std::is_constructible_v<UserClass, Args...>)
  {
    return SpawnBuilder<UserClass, /*kCreateFn=*/nullptr, Args...>(backend_actor_ref_, std::move(args)...)
        .ToNode(this_node_id_);
  }

  /** @copydoc ex_actor::Spawn */
  template <auto kCreateFn, class... Args>
  SenderOf<ActorRef<FnReturnType<kCreateFn>>> auto Spawn(Args... args)
    requires(std::is_invocable_v<decltype(kCreateFn), Args...>)
  {
    return SpawnBuilder<FnReturnType<kCreateFn>, kCreateFn, Args...>(backend_actor_ref_, std::move(args)...)
        .ToNode(this_node_id_);
  }

  template <class UserClass>
  SenderOf<> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
    return backend_actor_ref_.SendLocal<&ActorRegistryBackend::DestroyActor<UserClass>>(actor_ref);
  }

  /** @copydoc ex_actor::GetActorRefByName */
  template <class UserClass>
  SenderOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) const {
    // resolve overload ambiguity
    constexpr std::optional<ActorRef<UserClass>> (ActorRegistryBackend::*kProcessFn)(const std::string& name) const =
        &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return backend_actor_ref_.SendLocal<kProcessFn>(name);
  }

  /** @copydoc ex_actor::GetActorRefByName */
  template <class UserClass>
  SenderOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint64_t& node_id,
                                                                      const std::string& name) const {
    // resolve overload ambiguity
    constexpr ex::task<std::optional<ActorRef<UserClass>>> (ActorRegistryBackend::*kProcessFn)(
        const uint64_t& node_id, const std::string& name) const = &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return backend_actor_ref_.SendLocal<kProcessFn>(node_id, name);
  }

  SenderOf<WaitClusterStateResult> auto WaitClusterState(std::function<bool(const ClusterState&)> predicate,
                                                         uint64_t timeout_ms) {
    EXA_THROW_CHECK(!broker_actor_ref_.IsEmpty())
        << "WaitClusterState requires distributed mode, call StartOrJoinCluster() after Init() to enable it.";
    return broker_actor_ref_.SendLocal<&MessageBroker::WaitClusterState>(std::move(predicate), timeout_ms);
  }

  /** @copydoc ex_actor::StartOrJoinCluster */
  ex::task<void> StartOrJoinCluster(const ClusterConfig& cluster_config);

  /** @copydoc ex_actor::GetNodeId */
  uint64_t GetNodeId() const { return this_node_id_; }

 private:
  uint64_t this_node_id_;
  WorkSharingThreadPool default_work_sharing_thread_pool_;
  WorkSharingThreadPool control_plane_thread_pool_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  std::unique_ptr<Actor<MessageBroker>> broker_actor_;
  BasicActorRef<MessageBroker> broker_actor_ref_;
  Actor<ActorRegistryBackend> backend_actor_;
  BasicActorRef<ActorRegistryBackend> backend_actor_ref_;

  explicit ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler);
};

}  // namespace ex_actor::internal
