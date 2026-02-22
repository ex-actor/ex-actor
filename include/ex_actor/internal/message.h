// Copyright 2026 The ex_actor Authors.
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

#include <cstdint>
#include <string>
#include <vector>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal {

enum class NetworkRequestType : uint8_t {
  kActorCreationRequest = 0,
  kActorMethodCallRequest,
  kActorLookUpRequest,
};

enum class NetworkReplyType : uint8_t {
  kActorCreationReturn = 0,
  kActorCreationError,
  kActorMethodCallReturn,
  kActorMethodCallError,
  kActorLookUpReturn,
  kActorLookUpError,

};

template <class Tuple>
struct ActorCreationArgs {
  ActorConfig actor_config;
  Tuple args_tuple;
};

struct ActorCreationError {
  std::string error;
};

template <class Tuple>
struct ActorMethodCallArgs {
  Tuple args_tuple;
};

template <class T>
struct ActorMethodReturnValue {
  T return_value;
};

struct ActorMethodReturnError {
  std::string error;
};

template <>
struct ActorMethodReturnValue<void> {};

struct ActorLookUpRequest {
  std::string actor_name;
};

struct NodeState {
  enum class Liveness : uint8_t { kAlive = 0, kConnecting, kQuitting, kDead };
  Liveness liveness {Liveness::kAlive};
  uint64_t last_seen = 0;
  uint32_t node_id = 0;
  std::string address;
};

struct GossipMessage {
  std::vector<NodeState> node_states;
};

template <auto kFn, class Ctx>
auto DeserializeFnArgs(const uint8_t* data, size_t size, const Ctx& ctx) {
  using Sig = Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsTupleType>>(data, size, ctx);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsTupleType>>(data, size, ctx);
  }
}
}  // namespace ex_actor::internal