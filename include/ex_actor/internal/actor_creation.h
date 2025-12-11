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
#include <exception>
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
#include "ex_actor/internal/remote_handler_registry.h"
#include "ex_actor/internal/scheduler.h"
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"

namespace ex_actor::internal {

class ActorRegistryRequestProcessor {
 public:
  explicit ActorRegistryRequestProcessor(std::unique_ptr<TypeErasedActorScheduler> scheduler, uint32_t this_node_id,
                                         const std::vector<NodeInfo>& cluster_node_info,
                                         network::MessageBroker* message_broker)
      : is_distributed_mode_(!cluster_node_info.empty()),
        scheduler_(std::move(scheduler)),
        this_node_id_(this_node_id),
        message_broker_(message_broker) {
    logging::SetupProcessWideLoggingConfig();
    InitRandomNumGenerator();
    ValidateNodeInfo(cluster_node_info);
  }

  exec::task<void> AsyncDestroyAllActors() {
    exec::async_scope async_scope;
    // bulk destroy actors
    logger_->info("Sending destroy messages to actors");
    for (auto& [_, actor] : actor_id_to_actor_) {
      async_scope.spawn(actor->AsyncDestroy());
    }
    logger_->info("Waiting for actors to be destroyed");
    co_await async_scope.on_empty();
    logger_->info("All actors destroyed");
  }

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

  exec::task<void> HandleNetworkRequest(uint64_t received_request_id, network::ByteBufferType request_buffer) {
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

 private:
  std::shared_ptr<spdlog::logger> logger_ = logging::CreateLogger("ActorRegistryRequestProcessor");
  bool is_distributed_mode_ = false;
  std::mt19937 random_num_generator_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  uint32_t this_node_id_ = 0;
  std::unordered_map<uint32_t, std::string> node_id_to_address_;
  network::MessageBroker* message_broker_ = nullptr;
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unordered_map<std::string, std::uint64_t> actor_name_to_id_;

  void InitRandomNumGenerator() {
    std::random_device rd;
    random_num_generator_ = std::mt19937(rd());
  }

  uint64_t GenerateRandomActorId() {
    while (true) {
      auto id = random_num_generator_();
      if (!actor_id_to_actor_.contains(id)) {
        return id;
      }
    }
  }

  void ValidateNodeInfo(const std::vector<NodeInfo>& cluster_node_info) {
    for (const auto& node : cluster_node_info) {
      EXA_THROW_CHECK(!node_id_to_address_.contains(node.node_id)) << "Duplicate node id: " << node.node_id;
      node_id_to_address_[node.node_id] = node.address;
    }
  }

  serde::NetworkRequestType ParseMessageType(const network::ByteBufferType& buffer) {
    EXA_THROW_CHECK_LE(buffer.size(), 1) << "Invalid buffer size, " << buffer.size();
    return static_cast<serde::NetworkRequestType>(*static_cast<const uint8_t*>(buffer.data()));
  }

  void ReplyError(uint64_t received_request_id, serde::NetworkReplyType reply_type, std::string error_msg) {
    std::vector<char> serialized = serde::Serialize(serde::ActorMethodReturnValue {std::move(error_msg)});
    serde::BufferWriter writer(network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
    writer.WritePrimitive(reply_type);
    writer.CopyFrom(serialized.data(), serialized.size());
    message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
  }

  void HandleActorCreationRequest(uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader) {
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

  exec::task<void> HandleActorMethodCallRequest(uint64_t received_request_id,
                                                serde::BufferReader<network::ByteBufferType> reader) {
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
};

template <ex::scheduler Scheduler = WorkSharingThreadPool::Scheduler>
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
  explicit ActorRegistry(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config = {})
      : ActorRegistry(/*thread_pool_size=*/0, std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler), this_node_id,
                      cluster_node_info, heartbeat_config) {}

  ~ActorRegistry() {
    logger_->info("Start to shutdown actor registry");
    if (is_distributed_mode_) {
      message_broker_->ClusterAlignedStop();
    }
    ex::sync_wait(processor_actor_.CallActorMethod<&ActorRegistryRequestProcessor::AsyncDestroyAllActors>());
    ex::sync_wait(processor_actor_.AsyncDestroy());
    ex::sync_wait(async_scope_.on_empty());
    logger_->info("Actor registry shutdown completed");
  }

  /**
   * @brief Create actor at current node using default config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  reflect::AwaitableOf<ActorRef<UserClass>> auto CreateActor(Args... args) {
    // resolve overload ambiguity
    constexpr exec::task<ActorRef<UserClass>> (ActorRegistryRequestProcessor::*kProcessFn)(Args...) =
        &ActorRegistryRequestProcessor::CreateActor<UserClass, kCreateFn, Args...>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(std::move(args)...));
  }

  /**
   * @brief Create an actor with a manually specified config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  reflect::AwaitableOf<ActorRef<UserClass>> auto CreateActor(ActorConfig config, Args... args) {
    // resolve overload ambiguity
    constexpr exec::task<ActorRef<UserClass>> (ActorRegistryRequestProcessor::*kProcessFn)(ActorConfig, Args...) =
        &ActorRegistryRequestProcessor::CreateActor<UserClass, kCreateFn, Args...>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(config, std::move(args)...));
  }

  template <class UserClass>
  reflect::AwaitableOf<void> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
    return util::WrapSenderWithInlineScheduler(
        processor_actor_ref_.SendLocal<&ActorRegistryRequestProcessor::DestroyActor<UserClass>>(actor_ref));
  }

  /**
   * @brief Find the actor by name at current node.
   */
  template <class UserClass>
  reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) const {
    // resolve overload ambiguity
    constexpr std::optional<ActorRef<UserClass>> (ActorRegistryRequestProcessor::*kProcessFn)(const std::string& name)
        const = &ActorRegistryRequestProcessor::GetActorRefByName<UserClass>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(name));
  }

  /**
   * @brief Find the actor by name at remote node.
   */
  template <class UserClass>
  reflect::AwaitableOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint32_t& node_id,
                                                                                  const std::string& name) const {
    // resolve overload ambiguity
    constexpr exec::task<std::optional<ActorRef<UserClass>>> (ActorRegistryRequestProcessor::*kProcessFn)(
        const uint32_t& node_id, const std::string& name) const =
        &ActorRegistryRequestProcessor::GetActorRefByName<UserClass>;

    return util::WrapSenderWithInlineScheduler(processor_actor_ref_.SendLocal<kProcessFn>(node_id, name));
  }

  Scheduler GetScheduler() const {
    return *reinterpret_cast<const Scheduler*>(scheduler_->GetUnderlyingSchedulerPtr());
  }

 private:
  std::shared_ptr<spdlog::logger> logger_ = logging::CreateLogger("ActorRegistry");
  bool is_distributed_mode_;
  uint32_t this_node_id_;
  WorkSharingThreadPool default_work_sharing_thread_pool_;
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  std::unique_ptr<network::MessageBroker> message_broker_;
  Actor<ActorRegistryRequestProcessor> processor_actor_;
  ActorRef<ActorRegistryRequestProcessor> processor_actor_ref_;
  exec::async_scope async_scope_;

  explicit ActorRegistry(uint32_t thread_pool_size, std::unique_ptr<TypeErasedActorScheduler> scheduler,
                         uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config = {})
      : is_distributed_mode_(!cluster_node_info.empty()),
        this_node_id_(this_node_id),
        default_work_sharing_thread_pool_(thread_pool_size),
        scheduler_([&scheduler, this]() -> std::unique_ptr<TypeErasedActorScheduler> {
          return scheduler == nullptr ? std::make_unique<AnyStdExecScheduler<WorkSharingThreadPool::Scheduler>>(
                                            default_work_sharing_thread_pool_.GetScheduler())
                                      : std::move(scheduler);
        }()),
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
};
}  // namespace ex_actor::internal

namespace ex_actor {
using ex_actor::internal::ActorRegistry;
}  // namespace ex_actor
