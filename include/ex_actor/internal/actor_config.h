#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <stdexec/execution.hpp>

namespace ex_actor {
struct ActorConfig {
  size_t max_message_executed_per_activation = 100;
  uint32_t node_id = 0;
  std::optional<std::string> actor_name;
  std::optional<std::unordered_map<std::string, std::string>> std_exec_envs;
};

struct get_std_exec_env_t {
  constexpr std::optional<std::unordered_map<std::string, std::string>> operator()(const auto& prop) const noexcept {
    if constexpr (requires { prop.query(get_std_exec_env_t {}); }) {
      return prop.query(get_std_exec_env_t {});
    } else {
      return std::nullopt;
    }
  }
};
constexpr inline get_std_exec_env_t get_std_exec_env {};
}  // namespace ex_actor