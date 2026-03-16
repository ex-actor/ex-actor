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
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {
/**
 * @brief An actor wrapper of the most functions of ActorRegistry. Bootstraps itself as an actor, so it can reuse the
 * core functionality of Actors like mailboxes, schedulers, etc.
 *
 */
class ActorRegistryBackend {
 public:
  explicit ActorRegistryBackend(std::unique_ptr<TypeErasedActorScheduler> scheduler, uint64_t this_node_id,
                                LocalActorRef<MessageBroker> broker_actor_ref);

  exec::task<void> AsyncDestroyAllActors();

  /**
   * @brief Spawn a local actor with config.
   */
  template <class UserClass, class... Args>
  exec::task<ActorRef<UserClass>> SpawnWithClass(uint64_t node_id, ActorConfig config, Args... args) {
    if (node_id != this_node_id_) {
      EXA_THROW << "Spawn<UserClass> can only be used to create local actor, to create remote actor, use "
                   "Spawn<&CreateFn> to provide a fixed signature for remote actor creation. node_id="
                << node_id << ", this_node_id=" << this_node_id_ << ", actor_type=" << typeid(UserClass).name();
    }
    co_return co_await SpawnLocal<UserClass>(node_id, std::move(config), std::move(args)...);
  }

  /**
   * @brief Spawn an actor (local or remote) using a factory function, with config.
   */
  template <auto kCreateFn, class... Args>
  exec::task<ActorRef<FnReturnType<kCreateFn>>> SpawnWithCreateFn(uint64_t node_id, ActorConfig config, Args... args) {
    using UserClass = FnReturnType<kCreateFn>;
    static_assert(std::is_invocable_v<decltype(kCreateFn), Args...>,
                  "Class can't be created by given args and create function");

    if (node_id == this_node_id_) {
      co_return co_await SpawnLocal<UserClass, kCreateFn>(node_id, std::move(config), std::move(args)...);
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
      return ActorRef<UserClass>(this_node_id_, this_node_id_, actor_id, actor.get(), broker_actor_ref_);
    }
    return std::nullopt;
  }

  template <class UserClass>
  exec::task<std::optional<ActorRef<UserClass>>> GetActorRefByName(const uint64_t& node_id,
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
   * @brief Handle a network request. Returns the serialized reply data.
   */
  exec::task<ByteBuffer> HandleNetworkRequest(ByteBuffer request_buffer);

 private:
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  exec::task<ActorRef<UserClass>> SpawnLocal(uint64_t node_id, ActorConfig config, Args... args) {
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
    co_return handle;
  }

  std::mt19937 random_num_generator_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  uint64_t this_node_id_ = 0;
  LocalActorRef<MessageBroker> broker_actor_ref_;
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unordered_map<std::string, std::uint64_t> actor_name_to_id_;

  void InitRandomNumGenerator();
  uint64_t GenerateRandomActorId();
  ByteBuffer SerializeReply(const NetworkReply& reply);
  void HandleActorCreationRequest(ActorCreationRequest msg, ByteBuffer& reply_out);
  exec::task<ByteBuffer> HandleActorMethodCallRequest(ActorMethodCallRequest msg);
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

  explicit SpawnBuilder(LocalActorRef<ActorRegistryBackend> backend_ref, Args... args)
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
    return stdexec::connect(MakeSender(node_id_, config_, args_), std::move(receiver));
  }

  template <ex::receiver Receiver>
  auto connect(Receiver receiver) && {
    return stdexec::connect(MakeSender(node_id_, std::move(config_), std::move(args_)), std::move(receiver));
  }

 private:
  template <class Config, class Tuple>
  auto MakeSender(uint64_t node_id, Config&& config, Tuple&& args) {
    return std::apply(
        [this, node_id, config = std::forward<Config>(config)](auto&&... a) mutable {
          if constexpr (kCreateFn == nullptr) {
            return WrapSenderWithInlineScheduler(
                backend_ref_.template SendLocal<&ActorRegistryBackend::SpawnWithClass<UserClass, Args...>>(
                    node_id, std::move(config), std::forward<decltype(a)>(a)...));
          } else {
            return WrapSenderWithInlineScheduler(
                backend_ref_.template SendLocal<&ActorRegistryBackend::SpawnWithCreateFn<kCreateFn, Args...>>(
                    node_id, std::move(config), std::forward<decltype(a)>(a)...));
          }
        },
        std::forward<Tuple>(args));
  }

  LocalActorRef<ActorRegistryBackend> backend_ref_;
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
   * @brief Spawn a local actor. Returns a SpawnBuilder (which is a sender) that can optionally be configured
   * with .WithConfig() before awaiting.
   */
  template <class UserClass, class... Args>
  SenderOf<ActorRef<UserClass>> auto Spawn(Args... args) {
    return SpawnBuilder<UserClass, /*kCreateFn=*/nullptr, Args...>(backend_actor_ref_, std::move(args)...)
        .ToNode(this_node_id_);
  }

  /**
   * @brief Spawn an actor (local or remote) using a factory function. Returns a SpawnBuilder (which is a sender)
   * that can optionally be configured with .WithConfig() before awaiting.
   */
  template <auto kCreateFn, class... Args>
  SenderOf<ActorRef<FnReturnType<kCreateFn>>> auto Spawn(Args... args) {
    return SpawnBuilder<FnReturnType<kCreateFn>, kCreateFn, Args...>(backend_actor_ref_, std::move(args)...)
        .ToNode(this_node_id_);
  }

  template <class UserClass>
  SenderOf<> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
    return WrapSenderWithInlineScheduler(
        backend_actor_ref_.SendLocal<&ActorRegistryBackend::DestroyActor<UserClass>>(actor_ref));
  }

  /**
   * @brief Find the actor by name at current node.
   */
  template <class UserClass>
  SenderOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) const {
    // resolve overload ambiguity
    constexpr std::optional<ActorRef<UserClass>> (ActorRegistryBackend::*kProcessFn)(const std::string& name) const =
        &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(name));
  }

  /**
   * @brief Find the actor by name at remote node.
   */
  template <class UserClass>
  SenderOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint64_t& node_id,
                                                                      const std::string& name) const {
    // resolve overload ambiguity
    constexpr exec::task<std::optional<ActorRef<UserClass>>> (ActorRegistryBackend::*kProcessFn)(
        const uint64_t& node_id, const std::string& name) const = &ActorRegistryBackend::GetActorRefByName<UserClass>;

    return WrapSenderWithInlineScheduler(backend_actor_ref_.SendLocal<kProcessFn>(node_id, name));
  }

  exec::task<WaitNodeConditionResult> WaitNodeCondition(std::function<bool(const std::vector<NodeInfo>&)> predicate,
                                                        uint64_t timeout_ms);

 private:
  uint64_t this_node_id_;
  WorkSharingThreadPool default_work_sharing_thread_pool_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  std::unique_ptr<Actor<MessageBroker>> broker_actor_;
  LocalActorRef<MessageBroker> broker_actor_ref_;
  Actor<ActorRegistryBackend> backend_actor_;
  LocalActorRef<ActorRegistryBackend> backend_actor_ref_;

  explicit ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                         const ClusterConfig& cluster_config);
};

}  // namespace ex_actor::internal
