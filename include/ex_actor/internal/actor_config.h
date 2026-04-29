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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <stdexec/execution.hpp>

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep

namespace ex_actor {

enum class MailboxType : uint8_t {
  // the default type, suitable for most cases
  kUnboundedThreadSafeQueue,
  // very dangerous, should be paired with manually activation, used in some low-latency scenarios
  kBoundedUnsafeQueue,
};

struct MailboxConfig {
  MailboxType type = MailboxType::kUnboundedThreadSafeQueue;
  size_t mailbox_number = 1;
  // only used when queue type is bounded queue
  size_t mailbox_size = 0;
};

struct ActorConfig {
  size_t max_message_executed_per_activation = 100;

  MailboxConfig mailbox_config;

  /**
   * @brief Actor's name, should be unique within one node.
   *
   * @note GCC < 13 has a coroutine bug that double-frees temporaries containing heap-allocated fields
   * (like this std::optional<std::string>) in co_await expressions. Assign to a named variable first.
   * See docs/contents/installation.md "Known Issues: GCC before 13" for details and workarounds.
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
  constexpr auto query(ex::forwarding_query_t) const noexcept -> bool { return true; }
};

struct get_scheduler_index_t {
  constexpr size_t operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_scheduler_index_t {}); }) {
      return prop.query(get_scheduler_index_t {});
    } else {
      return 0;
    }
  }
  constexpr auto query(ex::forwarding_query_t) const noexcept -> bool { return true; }
};

constexpr inline get_priority_t get_priority {};
constexpr inline get_scheduler_index_t get_scheduler_index {};
}  // namespace ex_actor
