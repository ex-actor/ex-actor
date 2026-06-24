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

#include "ex_actor/internal/scheduler/core_pinned_thread_pool.h"

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/platform.h"

namespace ex_actor {

CorePinnedThreadPool::CorePinnedThreadPool(size_t thread_count) : thread_count_(thread_count), queues_(thread_count) {
  EXA_THROW_CHECK(thread_count > 0) << "Thread count must be greater than 0";
  for (size_t i = 0; i < thread_count; ++i) {
    workers_.emplace_back([this, i](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token, i); });
  }
}

void CorePinnedThreadPool::EnqueueOperation(TypeErasedOperation* operation, size_t core_index) {
  size_t queue_idx = core_index % thread_count_;
  queues_[queue_idx].Push(operation);
}

void CorePinnedThreadPool::WorkerThreadLoop(const std::stop_token& stop_token, size_t thread_index) {
  internal::SetThreadName(fmt_lib::format("cp_pool_worker_{}", thread_index));
  internal::SetThreadAffinity(thread_index);

  while (!stop_token.stop_requested()) {
    auto optional_operation = queues_[thread_index].Pop(/*timeout_ms=*/10);
    if (!optional_operation) {
      continue;
    }
    optional_operation.value()->Execute();
  }
}

}  // namespace ex_actor
