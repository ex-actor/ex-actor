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
namespace ex_actor {
namespace internal {

constexpr size_t kEmptyActorRefHashVal = 10086;
constexpr uint64_t kDefaultHeartbeatTimeoutMs = 5000;
constexpr uint64_t kDefaultGossipIntervalMs = 500;
constexpr size_t kDefaultGossipFanout = 3;

}  // namespace internal
}  // namespace ex_actor
