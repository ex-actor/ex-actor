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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <stdexec/execution.hpp>

#include "ex_actor/internal/constants.h"

namespace ex_actor {
struct ActorConfig {
  size_t max_message_executed_per_activation = 100;
  uint32_t node_id = 0;
  std::chrono::milliseconds remote_creation_timeout = internal::kDefaultRemoteCreateTimeout;

  /**
   * @brief Actor's name, should be unique within one node.
   *
   * @note Before gcc 13, we can't use heap-allocated temp variable after co_await, or there will be a double free
   * error. here actor_name is heap allocated. so when using ActorConfig with actor_name, we should define it
   * explicitly.
   *
   * i.e. you can't `co_await CreateActor<X>(ActorConfig {.actor_name = "xxx"})` directly, instead, you
   * should define a separate named variable for the config, and pass it to CreateActor(), like this:
   *
   * @code
   * ex_actor::ActorConfig config {.actor_name = "xxx"};
   * auto actor = co_await CreateActor<X>(config);
   * @endcode
   *
   * see gcc's bug report: https://gcc.gnu.org/pipermail/gcc-bugs/2022-October/800402.html
   */
  std::optional<std::string> actor_name;

  /*
  -----scheduler specific configs-----
  */

  // used in SchedulerUnion
  size_t scheduler_index = 0;
  // used in PriorityThreadPool
  uint32_t priority = UINT32_MAX;
};

struct get_priority_t {
  constexpr uint32_t operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_priority_t {}); }) {
      return prop.query(get_priority_t {});
    } else {
      return UINT32_MAX;
    }
  }
  constexpr auto query(stdexec::forwarding_query_t) const noexcept -> bool { return true; }
};

struct get_scheduler_index_t {
  constexpr size_t operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_scheduler_index_t {}); }) {
      return prop.query(get_scheduler_index_t {});
    } else {
      return 0;
    }
  }
  constexpr auto query(stdexec::forwarding_query_t) const noexcept -> bool { return true; }
};

constexpr inline get_priority_t get_priority {};
constexpr inline get_scheduler_index_t get_scheduler_index {};
}  // namespace ex_actor
