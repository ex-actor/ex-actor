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
#include <mutex>
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
#include "ex_actor/internal/serialization.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {

template <ex::scheduler Scheduler>
class ActorRegistry {
 public:
  /**
   * @brief Constructor for single-node mode.
   */
  explicit ActorRegistry(Scheduler scheduler) : scheduler_(std::move(scheduler)) {
    logging::SetupProcessWideLoggingConfig();
    InitRandomNumGenerator();
  }

  /**
   * @brief Constructor for distributed mode.
   */
  explicit ActorRegistry(Scheduler scheduler, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info,
                         network::HeartbeatConfig heartbeat_config =
                             {
                                 .heartbeat_timeout = kDefaultHeartbeatTimeout,
                                 .heartbeat_interval = kDefaultHeartbeatInterval,
                             })
      : is_distributed_mode_(true),
        scheduler_(std::move(scheduler)),
        this_node_id_(this_node_id),
        message_broker_(std::make_unique<network::MessageBroker>(
            cluster_node_info, this_node_id,
            [this](uint64_t received_request_id, network::ByteBufferType data) {
              HandleNetworkRequest(received_request_id, std::move(data));
            },
            heartbeat_config)) {
    logging::SetupProcessWideLoggingConfig();
    InitRandomNumGenerator();
    for (const auto& node : cluster_node_info) {
      EXA_THROW_CHECK(!node_id_to_address_.contains(node.node_id)) << "Duplicate node id: " << node.node_id;
      node_id_to_address_[node.node_id] = node.address;
    }
  }

  ~ActorRegistry() {
    // stop receiving network requests first
    if (is_distributed_mode_) {
      message_broker_->ClusterAlignedStop();
    }

    // bulk destroy actors
    auto destroy_msg = std::make_unique<DestroyMessage>();
    {
      auto& mutex = actor_id_to_actor_.GetMutex();
      std::lock_guard guard(mutex);
      auto& actor_id_to_actor = actor_id_to_actor_.GetMap();
      for (auto& [_, actor] : actor_id_to_actor) {
        actor->PushMessage(destroy_msg.get());
      }
    }

    ex::sync_wait(async_scope_.on_empty());
    actor_id_to_actor_.Clear();
  }

  /**
   * @brief Create actor at current node using default config.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> CreateActor(Args&&... args) {
    return CreateActor<UserClass>(ActorConfig {.node_id = this_node_id_}, std::forward<Args>(args)...);
  }

  /**
   * @brief Create an actor with a manually specified config.
   */
  template <class UserClass, auto kCreateFn = nullptr, class... Args>
  ActorRef<UserClass> CreateActor(ActorConfig config, Args&&... args) {
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
      auto actor = std::make_unique<Actor<UserClass, kCreateFn>>(
          std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler_), config, std::forward<Args>(args)...);
      auto handle = ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, actor.get(), message_broker_.get());
      if (config.actor_name.has_value()) {
        std::string& name = *config.actor_name;
        EXA_THROW_CHECK(!actor_name_to_id_.Contains(name))
            << "An actor with the same name already exists, name=" << name;
        actor_name_to_id_.Insert(name, actor_id);
      }
      actor_id_to_actor_.Insert(actor_id, std::move(actor));
      return handle;
    }

    if constexpr (kCreateFn == nullptr) {
      EXA_THROW << "CreateActor<UserClass> can only be used to create local actor, to create remote actor, use "
                   "CreateActor<UserClass, kCreateFn> to provide a fixed signature for remote actor creation. node_id="
                << config.node_id << ", this_node_id=" << this_node_id_ << ", actor_type=" << typeid(UserClass).name();
    }

    if constexpr (kCreateFn != nullptr) {
      using CreateFnSig = reflect::Signature<decltype(kCreateFn)>;

      // protocol: [message_type][handler_key_len][handler_key][ActorCreationArgs]
      typename CreateFnSig::DecayedArgsTupleType args_tuple {std::forward<Args>(args)...};
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
      auto sender =
          message_broker_->SendRequest(config.node_id, std::move(buffer_writer).MoveBufferOut()) |
          ex::then([](network::ByteBufferType response_buffer) {
            serde::BufferReader reader(std::move(response_buffer));
            auto type = reader.template NextPrimitive<serde::NetworkReplyType>();
            if (type == serde::NetworkReplyType::kActorCreationError) {
              EXA_THROW
                  << "Got actor creation error from remote node:"
                  << serde::Deserialize<serde::ActorCreationError>(reader.Current(), reader.RemainingSize()).error;
            }
            auto actor_id = reader.template NextPrimitive<uint64_t>();
            return actor_id;
          });
      // sync wait and create handle
      auto [actor_id] = stdexec::sync_wait(std::move(sender)).value();
      return ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, nullptr, message_broker_.get());
    }
  }

  template <class UserClass>
  void DestroyActor(const ActorRef<UserClass>& actor_ref) {
    auto actor_id = actor_ref.GetActorId();
    EXA_THROW_CHECK(actor_id_to_actor_.Contains(actor_id)) << "Actor with id " << actor_id << " not found";
    auto& actor = actor_id_to_actor_.At(actor_id);
    auto actor_name = actor->GetActorConfig().actor_name;
    actor_id_to_actor_.Erase(actor_id);
    if (actor_name.has_value()) {
      actor_name_to_id_.Erase(actor_name.value());
    }
  }

  template <class UserClass>
  std::optional<ActorRef<UserClass>> GetActorRefByName(const std::string& name) const {
    if (actor_name_to_id_.Contains(name)) {
      const auto actor_id = actor_name_to_id_.At(name);
      const auto& actor = actor_id_to_actor_.At(actor_id);
      return ActorRef<UserClass>(this_node_id_, this_node_id_, actor_id, actor.get(), message_broker_.get());
    }

    return std::nullopt;
  }

  template <class UserClass>
  std::optional<ActorRef<UserClass>> GetActorRefByName(const uint32_t& node_id, const std::string& name) const {
    if (node_id == this_node_id_) {
      return GetActorRefByName<UserClass>(name);
    }

    std::vector<char> serialized = serde::Serialize(serde::ActorLookUpRequest {name});
    serde::BufferWriter<network::ByteBufferType> writer(
        network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
    writer.WritePrimitive(serde::NetworkRequestType::kActorLookUpRequest);
    writer.CopyFrom(serialized.data(), serialized.size());

    auto sender = message_broker_->SendRequest(node_id, network::ByteBufferType {std::move(writer).MoveBufferOut()}) |
                  ex::then([](network::ByteBufferType buffer) -> std::optional<uint64_t> {
                    serde::BufferReader<network::ByteBufferType> reader(std::move(buffer));
                    auto type = reader.NextPrimitive<serde::NetworkReplyType>();
                    if (type == serde::NetworkReplyType::kActorLookUpReturn) {
                      return reader.NextPrimitive<uint64_t>();
                    }
                    return std::nullopt;
                  });

    auto [actor_id] = ex::sync_wait(std::move(sender)).value();
    if (actor_id.has_value()) {
      return ActorRef<UserClass>(this_node_id_, node_id, actor_id.value(), nullptr, message_broker_.get());
    }

    return std::nullopt;
  }

 private:
  bool is_distributed_mode_ = false;
  Scheduler scheduler_;
  std::mt19937 random_num_generator_;
  uint32_t this_node_id_ = 0;
  std::unordered_map<uint32_t, std::string> node_id_to_address_;
  util::LockGuardedMap<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unique_ptr<network::MessageBroker> message_broker_;
  exec::async_scope async_scope_;
  util::LockGuardedMap<std::string, std::uint64_t> actor_name_to_id_;

  void InitRandomNumGenerator() {
    std::random_device rd;
    random_num_generator_ = std::mt19937(rd());
  }

  uint64_t GenerateRandomActorId() {
    while (true) {
      auto id = random_num_generator_();
      if (!actor_id_to_actor_.Contains(id)) {
        return id;
      }
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

  void SetupActorRefSerializationContext() {
    auto& info = ActorRefDeserializationContext::GetThreadLocalInstance();
    info.this_node_id = this_node_id_;
    info.message_broker = message_broker_.get();
    info.actor_look_up_fn = [this](uint64_t actor_id) -> TypeErasedActor* {
      if (actor_id_to_actor_.Contains(actor_id)) {
        return actor_id_to_actor_.At(actor_id).get();
      }
      return nullptr;
    };
  }

  void HandleNetworkRequest(uint64_t received_request_id, network::ByteBufferType request_buffer) {
    SetupActorRefSerializationContext();
    serde::BufferReader<network::ByteBufferType> reader(std::move(request_buffer));
    auto message_type = reader.NextPrimitive<serde::NetworkRequestType>();

    if (message_type == serde::NetworkRequestType::kActorCreationRequest) {
      HandleActorCreationRequest(received_request_id, std::move(reader));
      return;
    }

    if (message_type == serde::NetworkRequestType::kActorMethodCallRequest) {
      HandleActorMethodCallRequest(received_request_id, std::move(reader));
      return;
    }

    if (message_type == serde::NetworkRequestType::kActorLookUpRequest) {
      auto actor_name =
          serde::Deserialize<serde::ActorLookUpRequest>(reader.Current(), reader.RemainingSize()).actor_name;

      if (actor_name_to_id_.Contains(actor_name)) {
        serde::BufferWriter<network::ByteBufferType> writer(
            network::ByteBufferType(sizeof(serde::NetworkRequestType) + sizeof(uint64_t)));
        auto actor_id = actor_name_to_id_.At(actor_name);
        writer.WritePrimitive(serde::NetworkReplyType::kActorLookUpReturn);
        writer.WritePrimitive(actor_id);
        message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
      } else {
        serde::BufferWriter writer(network::ByteBufferType(sizeof(serde::NetworkRequestType)));
        writer.WritePrimitive(serde::NetworkReplyType::kActorLookUpError);
        message_broker_->ReplyRequest(received_request_id, std::move(writer).MoveBufferOut());
      }

      return;
    }
    EXA_THROW << "Invalid message type: " << static_cast<int>(message_type);
  }

  void HandleActorCreationRequest(uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader) {
    auto handler_key_len = reader.NextPrimitive<uint64_t>();
    auto handler_key = reader.PullString(handler_key_len);
    try {
      auto handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorCreationHandler(handler_key);
      auto result = handler(RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext {
          .request_buffer = std::move(reader),
          .scheduler = std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler_),
      });
      uint64_t actor_id = GenerateRandomActorId();
      if (result.actor_name.has_value()) {
        EXA_THROW_CHECK(!actor_name_to_id_.Contains(result.actor_name.value()))
            << "An actor with the same name already exists, name=" << result.actor_name.value();
        actor_name_to_id_.Insert(result.actor_name.value(), actor_id);
      }
      actor_id_to_actor_.Insert(actor_id, std::move(result.actor));
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

  void HandleActorMethodCallRequest(uint64_t received_request_id, serde::BufferReader<network::ByteBufferType> reader) {
    auto handler_key_len = reader.NextPrimitive<uint64_t>();
    auto handler_key = reader.PullString(handler_key_len);
    auto actor_id = reader.NextPrimitive<uint64_t>();
    if (!actor_id_to_actor_.Contains(actor_id)) {
      ReplyError(
          received_request_id, serde::NetworkReplyType::kActorMethodCallError,
          fmt_lib::format("Can't find actor at remote node, actor_id={}, node_id={}, maybe it's already destroyed.",
                          actor_id, this_node_id_));
      return;
    }

    RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandler handler = nullptr;
    try {
      handler = RemoteActorRequestHandlerRegistry::GetInstance().GetRemoteActorMethodCallHandler(handler_key);
    } catch (std::exception& error) {
      auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
      ReplyError(received_request_id, serde::NetworkReplyType::kActorMethodCallError, std::move(error_msg));
      return;
    }

    EXA_THROW_CHECK(handler != nullptr);
    auto do_call =
        handler(RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext {
            .actor = actor_id_to_actor_.At(actor_id).get(),
            .request_buffer = std::move(reader),
        }) |
        ex::then([this, received_request_id](network::ByteBufferType buffer) {
          message_broker_->ReplyRequest(received_request_id, std::move(buffer));
        }) |
        ex::upon_error([this, received_request_id](auto error) {
          try {
            EXA_THROW_CHECK(error) << "Error should not be null";
            std::rethrow_exception(error);
          } catch (std::exception& error) {
            auto error_msg = fmt_lib::format("Exception type: {}, what(): {}", typeid(error).name(), error.what());
            ReplyError(received_request_id, serde::NetworkReplyType::kActorMethodCallError, std::move(error_msg));
          }
        });
    async_scope_.spawn(std::move(do_call));
  }
};
}  // namespace ex_actor::internal

namespace ex_actor {
using ex_actor::internal::ActorRegistry;
}  // namespace ex_actor
