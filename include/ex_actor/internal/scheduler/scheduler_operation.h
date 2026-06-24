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

#include <utility>

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep

namespace ex_actor::internal {

struct TypeErasedOperation {
  virtual ~TypeErasedOperation() = default;
  virtual void Execute() = 0;
};

// Base class for scheduler operations. Implements the common Execute() logic
// (stop-token check + set_value/set_stopped). Each pool defines a derived
// Operation that adds its own start() dispatching to the pool's queue.
template <typename Pool, ex::receiver R>
struct SchedulerOperationBase : TypeErasedOperation {
  SchedulerOperationBase(R receiver, Pool* thread_pool)
      : receiver(std::move(receiver)), thread_pool(thread_pool) {}

  R receiver;
  Pool* thread_pool;

  void Execute() override {
    auto env = ex::get_env(receiver);
    auto stoken = ex::get_stop_token(env);
    if constexpr (ex::unstoppable_token<decltype(stoken)>) {
      receiver.set_value();
    } else {
      if (stoken.stop_requested()) {
        receiver.set_stopped();
      } else {
        receiver.set_value();
      }
    }
  }
};

}  // namespace ex_actor::internal
