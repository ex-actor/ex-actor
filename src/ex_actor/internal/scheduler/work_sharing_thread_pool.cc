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

#include "ex_actor/internal/scheduler/work_sharing_thread_pool.h"

#include <utility>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/platform.h"

namespace ex_actor {

WorkSharingThreadPool::WorkSharingThreadPool(size_t thread_count, bool start_workers_immediately)
    : WorkSharingThreadPool(
          Config {.thread_count = thread_count, .start_workers_immediately = start_workers_immediately}) {}

WorkSharingThreadPool::WorkSharingThreadPool(Config config) : config_(std::move(config)) {
  if (config_.start_workers_immediately) {
    StartWorkers();
  }
}

void WorkSharingThreadPool::StartWorkers() {
  for (size_t i = 0; i < config_.thread_count; ++i) {
    workers_.emplace_back([this, i](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token, i); });
  }
}

void WorkSharingThreadPool::EnqueueOperation(TypeErasedOperation* operation) {
  if (owning_pool_ == this && local_slot_ == nullptr) {
    local_slot_ = operation;
    return;
  }
  queue_.Push(operation);
}

void WorkSharingThreadPool::WorkerThreadLoop(const std::stop_token& stop_token, size_t thread_index) {
  internal::SetThreadName("ws_pool_worker");
  if (!config_.cores.empty()) {
    size_t core_index = config_.cores[thread_index % config_.cores.size()];
    if (!internal::SetThreadAffinity(core_index)) {
      internal::log::Warn("Failed to pin work-sharing worker {} to core {}", thread_index, core_index);
    }
  }
  owning_pool_ = this;
  while (!stop_token.stop_requested()) {
    auto optional_operation = config_.spin ? queue_.TryPop() : queue_.Pop(/*timeout_ms=*/10);
    if (!optional_operation) {
      continue;
    }
    optional_operation.value()->Execute();
    while (local_slot_ != nullptr) {
      auto* operation = local_slot_;
      local_slot_ = nullptr;
      operation->Execute();
    }
  }
  owning_pool_ = nullptr;
}

}  // namespace ex_actor
