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

#include "ex_actor/internal/actor_ref.h"
#include "ex_actor/internal/actor_ref_serialization/actor_ref_serialization_reader.h"

namespace rfl {
template <typename U>
struct Reflector<ex_actor::internal::ActorRef<U>> {
  struct ReflType {
    bool is_valid {};
    uint32_t node_id {};
    uint64_t actor_id {};
  };

  static ex_actor::internal::ActorRef<U> to(const ReflType& rfl_type) noexcept {
    ex_actor::internal::ActorRef<U> actor(0, rfl_type.node_id, rfl_type.actor_id, nullptr, nullptr);
    return actor;
  }

  static ReflType from(const ex_actor::internal::ActorRef<U>& actor_ref) {
    return {
        .is_valid = actor_ref.is_empty_,
        .node_id = actor_ref.node_id_,
        .actor_id = actor_ref.actor_id_,
    };
  }
};

namespace parsing {
template <class ProcessorsType, class U>
struct Parser<capnproto::ActorRefReader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType> {
  using Reader = capnproto::ActorRefReader;
  using InputVarType = typename Reader::InputVarType;

  static Result<ex_actor::internal::ActorRef<U>> read(const Reader& reader, const InputVarType& var) noexcept {
    using Type = typename Reflector<ex_actor::internal::ActorRef<U>>::ReflType;
    auto actor_ref =
        Parser<capnproto::Reader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType>::read(reader, var)
            .value();
    const auto& info = reader.info;
    actor_ref.SetLocalRuntimeInfo(info.this_node_id, info.actor_look_up_fn(actor_ref.GetActorId()),
                                  info.message_broker);
    return actor_ref;
  }

  template <class Parent>
  static void write(const capnproto::Writer& w, const ex_actor::internal::ActorRef<U>& ref, const Parent& parent) {
    Parser<capnproto::Reader, capnproto::Writer, ex_actor::internal::ActorRef<U>, ProcessorsType>::write(w, ref,
                                                                                                         parent);
  }
};

}  // namespace parsing

}  // namespace rfl
