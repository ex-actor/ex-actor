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

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/serialization.h"

namespace ex_actor::internal {

// ===================================================
// Payload structs (used inside serialized_args/serialized_result)
// ===================================================

template <class Tuple>
struct ActorCreationArgs {
  ActorConfig actor_config;
  Tuple args_tuple;
};

template <class Tuple>
struct ActorMethodCallArgs {
  Tuple args_tuple;
};

template <class T>
struct ActorMethodReturnValue {
  T return_value;
};
template <>
struct ActorMethodReturnValue<void> {};

// ===================================================
// Typed network request messages (variant-based)
// ===================================================

struct ActorCreationRequest {
  std::string handler_key;
  ByteBuffer serialized_args;  // serialized ActorCreationArgs
};

struct ActorMethodCallRequest {
  std::string handler_key;
  uint64_t actor_id {};
  ByteBuffer serialized_args;  // serialized ActorMethodCallArgs
};

struct ActorLookUpRequest {
  std::string actor_name;
};

struct NetworkRequest {
  std::variant<ActorCreationRequest, ActorMethodCallRequest, ActorLookUpRequest> variant;
};

// ===================================================
// Typed network reply messages (variant-based)
// ===================================================

struct ActorCreationReply {
  bool success {};
  uint64_t actor_id {};
  std::string error;
};

struct ActorMethodCallReply {
  bool success {};
  ByteBuffer serialized_result;  // serialized ActorMethodReturnValue
  std::string error;
};

struct ActorLookUpReply {
  bool success {};
  uint64_t actor_id {};
};

struct NetworkReply {
  std::variant<ActorCreationReply, ActorMethodCallReply, ActorLookUpReply> variant;
};

// ===================================================
// Node states and gossip message
// ===================================================

struct NodeState {
  bool alive = true;
  uint64_t last_seen_timestamp_ms = 0;
  uint64_t node_id = 0;
  std::string address;
};

struct BrokerGossipMessage {
  uint64_t from_node_id;
  std::vector<NodeState> node_states;
};

struct BrokerTwoWayMessage {
  uint64_t request_node_id;
  uint64_t response_node_id;
  uint64_t request_id;  // the request id in the request node
  ByteBuffer payload;
};

struct BrokerMessage {
  std::variant<BrokerTwoWayMessage, BrokerGossipMessage> variant;
};

// ===================================================
// Util functions
// ===================================================

template <auto kFn, class Ctx>
auto DeserializeFnArgs(std::span<const std::byte> data, const Ctx& ctx) {
  using Sig = Signature<decltype(kFn)>;
  if constexpr (std::is_member_function_pointer_v<decltype(kFn)>) {
    return Deserialize<ActorMethodCallArgs<typename Sig::DecayedArgsTupleType>>(data, ctx);
  } else {
    return Deserialize<ActorCreationArgs<typename Sig::DecayedArgsTupleType>>(data, ctx);
  }
}
}  // namespace ex_actor::internal