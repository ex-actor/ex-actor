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

#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/actor_ref.h"
#include "ex_actor/internal/message.h"
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
    BufferReader<ByteBufferType> request_buffer;
    ActorRefSerdeContext info;
  };
  struct RemoteActorCreationHandlerContext {
    BufferReader<ByteBufferType> request_buffer;
    std::unique_ptr<TypeErasedActorScheduler> scheduler;
    ActorRefSerdeContext info;
  };
  struct CreateActorResult {
    std::unique_ptr<TypeErasedActor> actor;
    std::optional<std::string> actor_name;
  };

  using RemoteActorMethodCallHandler =
      std::function<exec::task<ByteBufferType>(RemoteActorMethodCallHandlerContext context)>;
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
    using CreateFnSig = Signature<decltype(kActorCreateFn)>;
    static_assert(!std::is_void_v<std::decay_t<typename CreateFnSig::ReturnType>>,
                  "kActorCreateFn must return a non-void value");
    static_assert(!std::is_fundamental_v<typename CreateFnSig::ReturnType>,
                  "kActorCreateFn must return a non-fundamental value");
    auto check_fn_class = []<class C, auto kMemberFn>() {
      using MemberFnSig = Signature<decltype(kMemberFn)>;
      static_assert(std::is_same_v<C, typename MemberFnSig::ClassType>,
                    "Actor methods' class does not match the create function's class");
    };
    (check_fn_class.template operator()<typename CreateFnSig::ReturnType, kActorMethods>(), ...);
    auto register_handler = [this]<auto kFuncPtr>() {
      std::string func_name = GetUniqueNameForFunction<kFuncPtr>();
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
    using ActorClass = Signature<decltype(kCreateFn)>::ReturnType;
    ActorCreationArgs creation_args = DeserializeFnArgs<kCreateFn>(
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
  exec::task<ByteBufferType> DeserializeAndInvokeActorMethod(
      RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
    EXA_THROW_CHECK(context.actor != nullptr);
    using Sig = Signature<decltype(kMethod)>;
    using UnwrappedType = decltype(UnwrapReturnSenderIfNested<kMethod>())::type;

    ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> call_args = DeserializeFnArgs<kMethod>(
        context.request_buffer.Current(), context.request_buffer.RemainingSize(), context.info);

    std::vector<char> serialized {};
    if constexpr (std::is_void_v<UnwrappedType>) {
      co_await context.actor->template CallActorMethodUseTuple<kMethod>(std::move(call_args.args_tuple));
    } else {
      auto return_value =
          co_await context.actor->template CallActorMethodUseTuple<kMethod>(std::move(call_args.args_tuple));
      serialized = Serialize(ActorMethodReturnValue<UnwrappedType> {std::move(return_value)});
    }

    BufferWriter writer(ByteBufferType {sizeof(NetworkRequestType) + serialized.size()});
    // TODO optimize the copy here
    writer.WritePrimitive(NetworkReplyType::kActorMethodCallReturn);
    if constexpr (!std::is_void_v<UnwrappedType>) {
      writer.CopyFrom(serialized.data(), serialized.size());
    }
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
