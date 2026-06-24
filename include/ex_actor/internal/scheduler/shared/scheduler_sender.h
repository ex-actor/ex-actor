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

#pragma once

#include <utility>

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep
#include "ex_actor/internal/scheduler/shared/stoppable_scheduler_completion_signatures.h"

namespace ex_actor::internal {

template <typename Pool>
struct SchedulerHandle;

template <typename Pool>
struct SchedulerSender : ex::sender_t, StoppableSchedulerCompletionSignatures {
  using StoppableSchedulerCompletionSignatures::get_completion_signatures;
  Pool* thread_pool;

  struct Env {
    Pool* thread_pool;
    template <class CPO>
    auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> SchedulerHandle<Pool> {
      return {.thread_pool = thread_pool};
    }
  };

  auto get_env() const noexcept -> Env { return Env {.thread_pool = thread_pool}; }

  template <ex::receiver R>
  typename Pool::template Operation<R> connect(R receiver) const {
    return {std::move(receiver), thread_pool};
  }
};

template <typename Pool>
struct SchedulerHandle : ex::scheduler_t {
  Pool* thread_pool;
  SchedulerSender<Pool> schedule() const noexcept { return {.thread_pool = thread_pool}; }
  friend bool operator==(const SchedulerHandle& lhs, const SchedulerHandle& rhs) noexcept {
    return lhs.thread_pool == rhs.thread_pool;
  }
};

}  // namespace ex_actor::internal
