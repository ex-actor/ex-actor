#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <stdexec/execution.hpp>

namespace ex_actor {
struct ActorConfig {
  size_t max_message_executed_per_activation = 100;
  uint32_t node_id = 0;
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
};

struct get_scheduler_index_t {
  constexpr uint32_t operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_scheduler_index_t {}); }) {
      return prop.query(get_scheduler_index_t {});
    } else {
      return 0;
    }
  }
};

constexpr inline get_priority_t get_priority {};
constexpr inline get_scheduler_index_t get_scheduler_index {};
}  // namespace ex_actor