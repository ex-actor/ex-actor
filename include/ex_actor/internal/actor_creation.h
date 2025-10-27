#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <random>
#include <type_traits>
#include <unordered_map>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/actor_ref.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal {

template <class... Classes>
struct ActorRoster {};

template <ex::scheduler Scheduler, class... ActorClasses>
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
                         ActorRoster<ActorClasses...> /*actor_roster*/)
      : is_distributed_mode_(true),
        scheduler_(std::move(scheduler)),
        this_node_id_(this_node_id),
        message_broker_(std::make_unique<network::MessageBroker>(
            cluster_node_info, this_node_id, [this](uint64_t receive_request_id, network::ByteBufferType data) {
              HandleNetworkRequest(receive_request_id, std::move(data));
            })) {
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
    for (auto& [_, actor] : actor_id_to_actor_) {
      actor->PushMessage(destroy_msg.get());
    }
    actor_id_to_actor_.clear();
  }

  /**
   * @brief Create an actor with a manually specified config.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> CreateActor(ActorConfig config, Args&&... args) {
    EXA_THROW_CHECK_EQ(config.node_id, this_node_id_)
        << "`CreteActor` can only be used to create actor at current node, to create actor at remote node, use "
           "`CreateActorUseStaticCreateFn`, because we need a unique construction signature.";
    auto actor_id = GenerateRandomActorId();
    auto actor =
        std::make_unique<Actor<UserClass, Scheduler>>(scheduler_, std::move(config), std::forward<Args>(args)...);
    auto handle = ActorRef<UserClass>(this_node_id_, config.node_id, actor_id,
                                      GetActorClassIndexInRoster<UserClass>().value_or(UINT64_MAX), actor.get(),
                                      message_broker_.get());
    actor_id_to_actor_[actor_id] = std::move(actor);
    return handle;
  }

  /**
   * @brief Create actor at current node using default config.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> CreateActor(Args&&... args) {
    return CreateActor<UserClass>(ActorConfig {.node_id = this_node_id_}, std::forward<Args>(args)...);
  }

  /**
   * @brief Create an actor using UserClass::Create() static method. This is for distributed mode, where the class
   * construction signature must be unique, or we don't know how to deserialize the args at the remote node.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> CreateActorUseStaticCreateFn(ActorConfig config, Args&&... args) {
    static_assert(std::is_invocable_v<decltype(&UserClass::Create), Args...>,
                  "Class can't be created by given args using UserClass::Create() static method");
    if (is_distributed_mode_) {
      EXA_THROW_CHECK(node_id_to_address_.contains(config.node_id)) << "Invalid node id: " << config.node_id;
    }

    // local actor, create directly
    if (config.node_id == this_node_id_) {
      auto actor_id = GenerateRandomActorId();
      auto actor = std::make_unique<Actor<UserClass, Scheduler, /*kUseStaticCreateFn=*/true>>(
          scheduler_, std::move(config), std::forward<Args>(args)...);
      auto handle = ActorRef<UserClass>(this_node_id_, config.node_id, actor_id,
                                        GetActorClassIndexInRoster<UserClass>().value_or(UINT64_MAX), actor.get(),
                                        message_broker_.get());
      actor_id_to_actor_[actor_id] = std::move(actor);
      return handle;
    }

    auto create_fn = &UserClass::Create;
    using CreateFnSig = reflect::Signature<decltype(create_fn)>;

    // remote actor, serialize args and send to remote
    uint64_t class_index_in_roster = GetActorClassIndexInRoster<UserClass>().value();

    // protocol: [message_type][class_index_in_roster][ActorCreationArgs]
    serde::ActorCreationArgs<typename CreateFnSig::DecayedArgsRflTupleType> actor_creation_args {
        config, typename CreateFnSig::DecayedArgsRflTupleType {std::forward<Args>(args)...}};
    std::vector<char> serialized = serde::Serialize(actor_creation_args);

    serde::BufferWriter buffer_writer(network::ByteBufferType {serialized.size() + sizeof(class_index_in_roster) +
                                                               sizeof(serde::NetworkRequestType)});

    buffer_writer.WritePrimitive(serde::NetworkRequestType::kActorCreationRequest);
    buffer_writer.WritePrimitive(class_index_in_roster);
    buffer_writer.CopyFrom(serialized.data(), serialized.size());  // TODO optimize the copy here
    EXA_THROW_CHECK(buffer_writer.EndReached()) << "Buffer writer not ended";

    // send to remote
    auto sender = message_broker_->SendRequest(config.node_id, std::move(buffer_writer).MoveBufferOut()) |
                  ex::then([](network::ByteBufferType response_buffer) {
                    serde::BufferReader reader(std::move(response_buffer));
                    auto actor_id = reader.template NextPrimitive<uint64_t>();
                    return actor_id;
                  });
    // sync wait and create handle
    auto [actor_id] = stdexec::sync_wait(std::move(sender)).value();
    return ActorRef<UserClass>(this_node_id_, config.node_id, actor_id, GetActorClassIndexInRoster<UserClass>().value(),
                               nullptr, message_broker_.get());
  }

  template <class UserClass>
  void DestroyActor(const ActorRef<UserClass>& actor_ref) {
    auto actor_id = actor_ref.GetActorId();
    EXA_THROW_CHECK(actor_id_to_actor_.contains(actor_id)) << "Actor with id " << actor_id << " not found";
    auto& actor = actor_id_to_actor_.at(actor_id);
    actor_id_to_actor_.erase(actor_id);
  }

 private:
  bool is_distributed_mode_ = false;
  Scheduler scheduler_;
  std::mt19937 random_num_generator_;
  uint32_t this_node_id_ = 0;
  std::unordered_map<uint32_t, std::string> node_id_to_address_;
  std::unordered_map<uint64_t, std::unique_ptr<TypeErasedActor>> actor_id_to_actor_;
  std::unique_ptr<network::MessageBroker> message_broker_;
  exec::async_scope async_scope_;

  template <class UserClass>
  std::optional<uint64_t> GetActorClassIndexInRoster() {
    constexpr auto actor_class_index = reflect::GetIndexInParamPack<UserClass, ActorClasses...>();
    if (is_distributed_mode_ && !actor_class_index.has_value()) {
      EXA_THROW_CHECK(actor_class_index.has_value())
          << "Can't find actor class in roster, class=" << typeid(UserClass).name();
    }
    return actor_class_index;
  }
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

  serde::NetworkRequestType ParseMessageType(const network::ByteBufferType& buffer) {
    EXA_THROW_CHECK_LE(buffer.size(), 1) << "Invalid buffer size, " << buffer.size();
    return static_cast<serde::NetworkRequestType>(*static_cast<const uint8_t*>(buffer.data()));
  }

  void HandleNetworkRequest(uint64_t receive_request_id, network::ByteBufferType request_buffer) {
    serde::BufferReader<network::ByteBufferType> reader(std::move(request_buffer));
    auto message_type = reader.NextPrimitive<serde::NetworkRequestType>();
    if (message_type == serde::NetworkRequestType::kActorCreationRequest) {
      auto actor_cls_index = reader.NextPrimitive<uint64_t>();
      uint64_t actor_id = FindClassAndCreateActor(actor_cls_index, reader.Current(), reader.RemainingSize());
      serde::BufferWriter<network::ByteBufferType> writer(network::ByteBufferType(sizeof(actor_id)));
      writer.WritePrimitive(actor_id);
      message_broker_->ReplyRequest(receive_request_id, std::move(writer).MoveBufferOut());
      return;
    }
    if (message_type == serde::NetworkRequestType::kActorMethodCallRequest) {
      auto class_index_in_roster = reader.NextPrimitive<uint64_t>();
      auto method_index = reader.NextPrimitive<uint64_t>();
      auto actor_id = reader.NextPrimitive<uint64_t>();
      FindClassAndCallMethod(class_index_in_roster, method_index, actor_id, receive_request_id, reader.Current(),
                             reader.RemainingSize());
      return;
    }
    EXA_THROW << "Invalid message type: " << static_cast<int>(message_type);
  }

  template <size_t kClassIndex = 0>
  uint64_t FindClassAndCreateActor(uint64_t actor_cls_index, const uint8_t* data, size_t size) {
    if (kClassIndex == actor_cls_index) {
      using ActorClass = reflect::ParamPackElement<kClassIndex, ActorClasses...>;
      constexpr auto kFactoryCreateFn = &ActorClass::Create;
      serde::ActorCreationArgs creation_args = serde::DeserializeFnArgs<kFactoryCreateFn>(data, size);
      std::unique_ptr<TypeErasedActor> actor = Actor<ActorClass, Scheduler>::CreateUseArgTuple(
          scheduler_, std::move(creation_args.actor_config), std::move(creation_args.args_tuple));
      auto actor_id = GenerateRandomActorId();
      actor_id_to_actor_[actor_id] = std::move(actor);
      return actor_id;
    }
    if constexpr (kClassIndex + 1 < sizeof...(ActorClasses)) {
      return FindClassAndCreateActor<kClassIndex + 1>(actor_cls_index, data, size);
    }
    EXA_THROW << "Can't find actor class in roster, actor_cls_index=" << actor_cls_index;
  }

  template <class ActorClass, size_t kMethodIndex = 0>
  void FindMethodAndCallIt(uint64_t method_index, uint64_t actor_id, uint64_t receive_request_id, const uint8_t* data,
                           size_t size) {
    constexpr auto kActorMethodsTuple = reflect::GetActorMethodsTuple<ActorClass>();
    if constexpr (std::tuple_size_v<decltype(kActorMethodsTuple)> > 0) {
      if (kMethodIndex == method_index) {
        std::unique_ptr<TypeErasedActor>& actor = actor_id_to_actor_.at(actor_id);
        constexpr auto kMethodPtr = std::get<kMethodIndex>(kActorMethodsTuple);
        using Sig = reflect::Signature<decltype(kMethodPtr)>;
        serde::ActorMethodCallArgs<typename Sig::DecayedArgsRflTupleType> call_args =
            serde::DeserializeFnArgs<kMethodPtr>(data, size);
        // TODO process ActorRef in the args
        auto sender = actor->template CallActorMethodUseTuple<kMethodPtr>(std::move(call_args.args_tuple)) |
                      ex::then([this, receive_request_id](auto return_value) {
                        std::vector<char> serialized =
                            serde::Serialize(serde::ActorMethodReturnValue {std::move(return_value)});
                        serde::BufferWriter writer(
                            network::ByteBufferType {sizeof(serde::NetworkRequestType) + serialized.size()});
                        // TODO optimize the copy here
                        writer.WritePrimitive(serde::NetworkRequestType::kActorMethodCallReturn);
                        writer.CopyFrom(serialized.data(), serialized.size());
                        message_broker_->ReplyRequest(receive_request_id, std::move(writer).MoveBufferOut());
                      }) |
                      ex::upon_error([this, receive_request_id](auto error) {
                        try {
                          if (error) {
                            std::rethrow_exception(error);
                          }
                        } catch (std::exception& error) {
                          std::vector<char> serialized =
                              serde::Serialize(serde::ActorMethodReturnValue {std::string(error.what())});
                          serde::BufferWriter writer(
                              network::ByteBufferType(sizeof(serde::NetworkRequestType) + serialized.size()));
                          writer.WritePrimitive(serde::NetworkRequestType::kActorMethodCallError);
                          writer.CopyFrom(serialized.data(), serialized.size());
                          message_broker_->ReplyRequest(receive_request_id, std::move(writer).MoveBufferOut());
                        }
                      });
        async_scope_.spawn(std::move(sender));
        return;
      }
    }
    if constexpr (kMethodIndex + 1 < std::tuple_size_v<decltype(kActorMethodsTuple)>) {
      return FindMethodAndCallIt<ActorClass, kMethodIndex + 1>(method_index, actor_id, receive_request_id, data, size);
    }
    EXA_THROW << "Can't find method in actor class, method_index=" << method_index
              << ", class=" << typeid(ActorClass).name();
  }

  template <size_t kClassIndex = 0>
  void FindClassAndCallMethod(uint64_t actor_cls_index, uint64_t method_index, uint64_t actor_id,
                              uint64_t receive_request_id, const uint8_t* data, size_t size) {
    if (kClassIndex == actor_cls_index) {
      using ActorClass = reflect::ParamPackElement<kClassIndex, ActorClasses...>;
      return FindMethodAndCallIt<ActorClass>(method_index, actor_id, receive_request_id, data, size);
    }
    if constexpr (kClassIndex + 1 < sizeof...(ActorClasses)) {
      return FindClassAndCallMethod<kClassIndex + 1>(actor_cls_index, method_index, actor_id, receive_request_id, data,
                                                     size);
    }
    EXA_THROW << "Can't find actor class in roster, actor_cls_index=" << actor_cls_index;
  }
};
}  // namespace ex_actor::internal

namespace ex_actor {
using ex_actor::internal::ActorRegistry;
using ex_actor::internal::ActorRoster;
}  // namespace ex_actor
