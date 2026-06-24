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

#include "ex_actor/internal/platform.h"

namespace ex_actor {

WorkSharingThreadPool::WorkSharingThreadPool(size_t thread_count, bool start_workers_immediately)
    : thread_count_(thread_count) {
  if (thread_count > 0 && start_workers_immediately) {
    StartWorkers();
  }
}

void WorkSharingThreadPool::StartWorkers() {
  for (size_t i = 0; i < thread_count_; ++i) {
    workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
  }
}

void WorkSharingThreadPool::EnqueueOperation(TypeErasedOperation* operation) {
  if (owning_pool_ == this && local_slot_ == nullptr) {
    local_slot_ = operation;
    return;
  }
  queue_.Push(operation);
}

void WorkSharingThreadPool::WorkerThreadLoop(const std::stop_token& stop_token) {
  internal::SetThreadName("ws_pool_worker");
  owning_pool_ = this;
  while (!stop_token.stop_requested()) {
    auto optional_operation = queue_.Pop(/*timeout_ms=*/10);
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
