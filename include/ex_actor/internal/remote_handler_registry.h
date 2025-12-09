#pragma once

#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/actor_ref_serialization/actor_ref_serialization.h"  // IWYU pragma: keep
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal {

class RemoteActorRequestHandlerRegistry {
 public:
  static RemoteActorRequestHandlerRegistry& GetInstance() {
    static RemoteActorRequestHandlerRegistry instance;
    return instance;
  }

  struct RemoteActorMethodCallHandlerContext {
    TypeErasedActor* actor;
    serde::BufferReader<network::ByteBufferType> request_buffer;
    ActorRefDeserializationInfo info;
  };
  struct RemoteActorCreationHandlerContext {
    serde::BufferReader<network::ByteBufferType> request_buffer;
    std::unique_ptr<TypeErasedActorScheduler> scheduler;
    ActorRefDeserializationInfo info;
  };
  struct CreateActorResult {
    std::unique_ptr<TypeErasedActor> actor;
    std::optional<std::string> actor_name;
  };

  using RemoteActorMethodCallHandler =
      std::function<exec::task<network::ByteBufferType>(RemoteActorMethodCallHandlerContext context)>;
  using RemoteActorCreationHandler = std::function<CreateActorResult(RemoteActorCreationHandlerContext context)>;

  void RegisterRemoteActorMethodCallHandler(const std::string& key, RemoteActorMethodCallHandler func) {
    EXA_THROW_CHECK(!remote_actor_method_call_handler_.contains(key))
        << "Duplicate remote actor method call handler: " << key
        << ", maybe you have duplicated functions in EXA_REMOTE.";
    remote_actor_method_call_handler_[key] = std::move(func);
  }

  RemoteActorMethodCallHandler GetRemoteActorMethodCallHandler(const std::string& key) const {
    EXA_THROW_CHECK(remote_actor_method_call_handler_.contains(key))
        << "Remote actor method call handler not found: " << key
        << ", maybe you forgot to register it with EXA_REMOTE.";
    return remote_actor_method_call_handler_.at(key);
  }
  void RegisterRemoteActorCreationHandler(const std::string& key, RemoteActorCreationHandler func) {
    EXA_THROW_CHECK(!remote_actor_creation_handler_.contains(key))
        << "Duplicate remote actor creation handler: " << key << ", maybe you have duplicated functions in EXA_REMOTE.";
    remote_actor_creation_handler_[key] = std::move(func);
  }
  RemoteActorCreationHandler GetRemoteActorCreationHandler(const std::string& key) const {
    EXA_THROW_CHECK(remote_actor_creation_handler_.contains(key))
        << "Remote actor creation handler not found: " << key << ", maybe you forgot to register it with EXA_REMOTE.";
    return remote_actor_creation_handler_.at(key);
  }

 private:
  std::unordered_map<std::string, RemoteActorCreationHandler> remote_actor_creation_handler_;
  std::unordered_map<std::string, RemoteActorMethodCallHandler> remote_actor_method_call_handler_;
};

template <auto kActorCreateFn, auto... kActorMethods>
class RemoteFuncHandlerRegistrar {
 public:
  explicit RemoteFuncHandlerRegistrar() {
    static_assert(!std::is_member_function_pointer_v<decltype(kActorCreateFn)>,
                  "kActorCreateFn must be a non-member function pointer");
    using CreateFnSig = reflect::Signature<decltype(kActorCreateFn)>;
    static_assert(!std::is_void_v<std::decay_t<typename CreateFnSig::ReturnType>>,
                  "kActorCreateFn must return a non-void value");
    static_assert(!std::is_fundamental_v<typename CreateFnSig::ReturnType>,
                  "kActorCreateFn must return a non-fundamental value");
    auto check_fn_class = []<class C, auto kMemberFn>() {
      using MemberFnSig = reflect::Signature<decltype(kMemberFn)>;
      static_assert(std::is_same_v<C, typename MemberFnSig::ClassType>,
                    "Actor methods' class does not match the create function's class");
    };
    (check_fn_class.template operator()<typename CreateFnSig::ReturnType, kActorMethods>(), ...);
    auto register_handler = [this]<auto kFuncPtr>() {
      std::string func_name = reflect::GetUniqueNameForFunction<kFuncPtr>();
      if constexpr (std::is_member_function_pointer_v<decltype(kFuncPtr)>) {
        RemoteActorRequestHandlerRegistry::GetInstance().RegisterRemoteActorMethodCallHandler(
            func_name, [this](RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
              return DeserializeAndInvokeActorMethod<kFuncPtr>(std::move(context));
            });
      } else {
        RemoteActorRequestHandlerRegistry::GetInstance().RegisterRemoteActorCreationHandler(
            func_name, [this](RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext context) {
              return DeserializeAndCreateActor<kFuncPtr>(std::move(context));
            });
      }
    };
    register_handler.template operator()<kActorCreateFn>();
    (register_handler.template operator()<kActorMethods>(), ...);
  }

 private:
  template <auto kCreateFn>
  RemoteActorRequestHandlerRegistry::CreateActorResult DeserializeAndCreateActor(
      RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext context) {
    using ActorClass = reflect::Signature<decltype(kCreateFn)>::ReturnType;
    serde::ActorCreationArgs creation_args = serde::DeserializeFnArgs<kCreateFn>(
        context.request_buffer.Current(), context.request_buffer.RemainingSize(), context.info);
    std::unique_ptr<TypeErasedActor> actor = Actor<ActorClass, kCreateFn>::CreateUseArgTuple(
        std::move(context.scheduler), std::move(creation_args.actor_config), std::move(creation_args.args_tuple));
    auto actor_name = actor->GetActorConfig().actor_name;
    return {
        .actor = std::move(actor),
        .actor_name = std::move(actor_name),
    };
  }

  /**
   * @returns A coroutine carrying the serialized result of the actor method call.
   */
  template <auto kMethod>
  exec::task<network::ByteBufferType> DeserializeAndInvokeActorMethod(
      RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
    EXA_THROW_CHECK(context.actor != nullptr);
    using Sig = reflect::Signature<decltype(kMethod)>;

    serde::ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> call_args = serde::DeserializeFnArgs<kMethod>(
        context.request_buffer.Current(), context.request_buffer.RemainingSize(), context.info);

    auto return_value =
        co_await context.actor->template CallActorMethodUseTuple<kMethod>(std::move(call_args.args_tuple));
    std::vector<char> serialized = serde::Serialize(serde::ActorMethodReturnValue {std::move(return_value)});
    serde::BufferWriter writer(network::ByteBufferType {sizeof(serde::NetworkRequestType) + serialized.size()});
    // TODO optimize the copy here
    writer.WritePrimitive(serde::NetworkReplyType::kActorMethodCallReturn);
    writer.CopyFrom(serialized.data(), serialized.size());
    co_return std::move(writer).MoveBufferOut();
  };
};

#define EXA_CONCATENATE_DIRECT(s1, s2, s3, s4) s1##s2##s3##s4
#define EXA_CONCATENATE(s1, s2, s3, s4) EXA_CONCATENATE_DIRECT(s1, s2, s3, s4)
#define EXA_ANONYMOUS_VARIABLE(str) EXA_CONCATENATE(str, __LINE__, _, __COUNTER__)
}  // namespace ex_actor::internal

namespace ex_actor {
#define EXA_REMOTE(...)                                                                        \
  /* NOLINTNEXTLINE(misc-use-internal-linkage) */                                              \
  inline ::ex_actor::internal::RemoteFuncHandlerRegistrar<__VA_ARGS__> EXA_ANONYMOUS_VARIABLE( \
      exa_remote_func_registrar_);
};  // namespace ex_actor
