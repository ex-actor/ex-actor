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
    ByteBuffer serialized_args;
    ActorRefSerdeContext actor_ref_serde_ctx;
  };
  struct RemoteActorCreationHandlerContext {
    ByteBuffer serialized_args;
    std::unique_ptr<TypeErasedActorScheduler> scheduler;
    ActorRefSerdeContext actor_ref_serde_ctx;
    uint64_t actor_id;
  };
  struct ActorCreationResult {
    std::unique_ptr<TypeErasedActor> actor;
    std::optional<std::string> actor_name;
    ByteBuffer serialized_actor_ref;
  };

  using RemoteActorMethodCallHandler =
      std::function<ex::task<NetworkReply>(RemoteActorMethodCallHandlerContext context)>;
  using RemoteActorCreationHandler = std::function<ActorCreationResult(RemoteActorCreationHandlerContext context)>;

  void RegisterRemoteActorMethodCallHandler(const std::string& handler_key, uint64_t actor_type_hash,
                                            RemoteActorMethodCallHandler func) {
    auto combined_key = std::make_pair(handler_key, actor_type_hash);
    EXA_THROW_CHECK(!remote_actor_method_call_handler_.contains(combined_key))
        << "Duplicate remote actor method call handler: " << handler_key << " for actor type hash " << actor_type_hash
        << ", maybe you have duplicated functions in EXA_REMOTE.";
    remote_actor_method_call_handler_[combined_key] = std::move(func);
  }

  RemoteActorMethodCallHandler GetRemoteActorMethodCallHandler(const std::string& handler_key,
                                                               uint64_t actor_type_hash) const {
    auto combined_key = std::make_pair(handler_key, actor_type_hash);
    EXA_THROW_CHECK(remote_actor_method_call_handler_.contains(combined_key))
        << "Remote actor method call handler not found: " << handler_key << " for actor type hash " << actor_type_hash
        << ", maybe you forgot to register it with EXA_REMOTE.";
    return remote_actor_method_call_handler_.at(combined_key);
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
  struct PairHash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
      std::size_t h1 = std::hash<T1> {}(p.first);
      std::size_t h2 = std::hash<T2> {}(p.second);
      return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
  };

  std::unordered_map<std::string, RemoteActorCreationHandler> remote_actor_creation_handler_;
  std::unordered_map<std::pair<std::string, uint64_t>, RemoteActorMethodCallHandler, PairHash>
      remote_actor_method_call_handler_;
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
      static_assert(std::is_base_of_v<typename MemberFnSig::ClassType, C>,
                    "Actor methods' class does not match the create function's class");
    };
    (check_fn_class.template operator()<typename CreateFnSig::ReturnType, kActorMethods>(), ...);
    auto register_handler = [this]<auto kFuncPtr>() {
      if constexpr (std::is_member_function_pointer_v<decltype(kFuncPtr)>) {
        using ActorClass = typename CreateFnSig::ReturnType;
        std::string func_name = GetUniqueNameForFunction<kFuncPtr>();
        RemoteActorRequestHandlerRegistry::GetInstance().RegisterRemoteActorMethodCallHandler(
            func_name, GetHashValue<ActorClass>(),
            [this](RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
              return DeserializeAndInvokeActorMethod<ActorClass, kFuncPtr>(std::move(context));
            });
      } else {
        std::string func_name = GetUniqueNameForFunction<kFuncPtr>();
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
  RemoteActorRequestHandlerRegistry::ActorCreationResult DeserializeAndCreateActor(
      RemoteActorRequestHandlerRegistry::RemoteActorCreationHandlerContext context) {
    using ActorClass = Signature<decltype(kCreateFn)>::ReturnType;
    ActorCreationArgs creation_args =
        DeserializeFnArgs<kCreateFn>(context.serialized_args, context.actor_ref_serde_ctx);
    std::unique_ptr<TypeErasedActor> actor = Actor<ActorClass, kCreateFn>::CreateUseArgTuple(
        std::move(context.scheduler), std::move(creation_args.actor_config), std::move(creation_args.args_tuple));
    auto this_node_id = context.actor_ref_serde_ctx.this_node_id;
    auto actor_ref = ActorRef<ActorClass>(this_node_id, this_node_id, context.actor_id, actor.get(),
                                          context.actor_ref_serde_ctx.broker_actor_ref);
    NotifyOnSpawned<ActorClass>(actor.get(), actor_ref);
    auto actor_name = actor->GetActorConfig().actor_name;
    return {
        .actor = std::move(actor),
        .actor_name = std::move(actor_name),
        // Workaround: see ActorRegistryBackend::DeserializeActorRef comment.
        .serialized_actor_ref = Serialize(rfl::Reflector<ActorRef<ActorClass>>::from(actor_ref)),
    };
  }

  /**
   * @returns A coroutine carrying the serialized result of the actor method call.
   */
  template <class ActualClass, auto kMethod>
  ex::task<NetworkReply> DeserializeAndInvokeActorMethod(
      RemoteActorRequestHandlerRegistry::RemoteActorMethodCallHandlerContext context) {
    EXA_THROW_CHECK(context.actor != nullptr);
    using Sig = Signature<decltype(kMethod)>;
    using UnwrappedType = decltype(UnwrapReturnSenderIfNested<kMethod>())::type;

    ActorMethodCallArgs<typename Sig::DecayedArgsTupleType> call_args =
        DeserializeFnArgs<kMethod>(context.serialized_args, context.actor_ref_serde_ctx);

    using MethodClass = typename Sig::ClassType;
    void* instance_address = context.actor->GetUserClassInstanceAddress();
    auto* actual_class_ptr = static_cast<ActualClass*>(instance_address);
    auto* adjusted_ptr = static_cast<MethodClass*>(actual_class_ptr);

    if constexpr (std::is_void_v<UnwrappedType>) {
      co_await CallActorMethodUseTuple<kMethod>(context.actor, adjusted_ptr, std::move(call_args.args_tuple));
      co_return NetworkReply {ActorMethodCallReply {.success = true}};
    } else {
      auto return_value =
          co_await CallActorMethodUseTuple<kMethod>(context.actor, adjusted_ptr, std::move(call_args.args_tuple));
      co_return NetworkReply {ActorMethodCallReply {
          .success = true,
          .serialized_result = Serialize(ActorMethodReturnValue<UnwrappedType> {std::move(return_value)})}};
    }
  };
};

#define EXA_CONCATENATE_DIRECT(s1, s2, s3, s4) s1##s2##s3##s4
#define EXA_CONCATENATE(s1, s2, s3, s4) EXA_CONCATENATE_DIRECT(s1, s2, s3, s4)
#define EXA_ANONYMOUS_VARIABLE(str) EXA_CONCATENATE(str, __LINE__, _, __COUNTER__)
}  // namespace ex_actor::internal

#define EXA_REMOTE(...)                                                                        \
  /* NOLINTNEXTLINE(misc-use-internal-linkage) */                                              \
  inline ::ex_actor::internal::RemoteFuncHandlerRegistrar<__VA_ARGS__> EXA_ANONYMOUS_VARIABLE( \
      exa_remote_func_registrar_);
