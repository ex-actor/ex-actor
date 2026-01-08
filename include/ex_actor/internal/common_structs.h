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

namespace ex_actor {

/// the event passed to user's hook function in manual actor activation.
struct MessagePushEvent {
  /// index of the mailbox the message was pushed to.
  size_t mailbox_index = 0;
  /// whether the message is a destroy message, when it's true, all other fields are invalid.
  /// when a destroy message is pushed, the actor will be marked as pending to be destroyed.
  /// this flag will be checked every time the actor is activated. Once found, the actor will be destroyed.
  ///
  /// @attention you should make sure the actor will be activated at least once after this event. Or the actor will
  /// never be destroyed, and you'll possible block forever when shutting down.
  bool is_destroy_message = false;
};

/// @brief Actor configuration, passed to ex_actor::Spawn.
/// @attention If you meet segmentation fault(double free) in destructor of ActorConfig, read the following details.
///
/// Before gcc 13, we can't use heap-allocated temp variable after co_await, or there will be a double free error. So
/// when using ActorConfig before gcc 13, we must give it a variable name(lvalue);
///
/// i.e. you can't `co_await CreateActor<X>(ActorConfig {...})`, instead, you should define a
/// separate named variable for the config, and copy it to CreateActor(), like this:
///
/// ```cpp
/// ex_actor::ActorConfig config {...}; // must give it a variable name(lvalue)
/// auto actor = co_await CreateActor<X>(config);
/// ```
///
/// see gcc's bug report: https://gcc.gnu.org/pipermail/gcc-bugs/2022-October/800402.html
struct ActorConfig {
  size_t max_message_executed_per_activation = 100;
  uint32_t node_id = 0;

  /// actor's name, should be unique within one node.
  std::optional<std::string> actor_name;

  // ------------ mailbox related configs -----------

  /// default mailbox type, a thread-safe unbounded queue.
  struct UnboundedThreadSafeMailboxConfig {};

  /// a mailbox with only one slot, not thread-safe.
  ///
  /// ususally combined with manually actor activation and multiple mailboxes. useful when you want to precisely
  /// control the actor activation and get rid of the mailbox's synchronization overhead.
  struct OneSlotUnsafeMailboxConfig {};

  using MailboxConfig = std::variant<UnboundedThreadSafeMailboxConfig, OneSlotUnsafeMailboxConfig>;

  /// configs of each mailbox, the number of mailboxes is the size of this vector.
  std::vector<MailboxConfig> mailbox_configs = {UnboundedThreadSafeMailboxConfig {}};

  //-----scheduler specific configs(experimental, might be changed in the future)-----

  /// used in SchedulerUnion
  size_t scheduler_index = 0;
  /// used in PriorityThreadPool
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