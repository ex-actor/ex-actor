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
#include <cstdlib>
#include <string>
#include <type_traits>

namespace ex_actor {
namespace internal {

/// Read an environment variable as type T, returning `default_val` when the
/// variable is unset or empty.
/// Supported types: std::string, double/float, and integral types.
template <typename T>
inline T GetEnv(const char* name, T default_val) {
  const char* val = std::getenv(name);
  if (val == nullptr || val[0] == '\0') {
    return default_val;
  }
  if constexpr (std::is_same_v<T, std::string>) {
    return std::string(val);
  } else if constexpr (std::is_same_v<T, double>) {
    return std::strtod(val, nullptr);
  } else if constexpr (std::is_same_v<T, float>) {
    return std::strtof(val, nullptr);
  } else if constexpr (std::is_signed_v<T>) {
    return static_cast<T>(std::strtoll(val, nullptr, 10));
  } else {
    return static_cast<T>(std::strtoull(val, nullptr, 10));
  }
}

constexpr size_t kEmptyActorRefHashVal = 10086;

// Env-configurable constants
inline const uint64_t kDefaultHeartbeatTimeoutMs = GetEnv<uint64_t>("EXA_HEARTBEAT_TIMEOUT_MS", 10000);
inline const uint64_t kDefaultGossipIntervalMs = GetEnv<uint64_t>("EXA_GOSSIP_INTERVAL_MS", 500);
inline const size_t kDefaultGossipFanout = GetEnv<size_t>("EXA_GOSSIP_FANOUT", 3);
inline const uint64_t kDefaultHeartbeatCheckIntervalMs = GetEnv<uint64_t>("EXA_HEARTBEAT_CHECK_INTERVAL_MS", 100);
inline const uint64_t kDefaultWaiterExpirationCheckIntervalMs =
    GetEnv<uint64_t>("EXA_WAITER_EXPIRATION_CHECK_INTERVAL_MS", 100);

}  // namespace internal
}  // namespace ex_actor
