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

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep

namespace ex_actor::internal {

// Mixin that provides environment-adaptive completion signatures for scheduler senders.
// When the environment has a never_stop_token, advertises only set_value_t(); otherwise
// also advertises set_stopped_t().
struct StoppableSchedulerCompletionSignatures {
  template <class Self>
  static consteval auto get_completion_signatures() {
    return ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>();
  }

  template <class Self, class Env>
  static consteval auto get_completion_signatures() {
    if constexpr (ex::unstoppable_token<ex::stop_token_of_t<Env>>) {
      return ex::completion_signatures<ex::set_value_t()>();
    } else {
      return ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>();
    }
  }
};

}  // namespace ex_actor::internal
