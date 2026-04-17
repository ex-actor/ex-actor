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

#include <atomic>
#include <mutex>
#include <source_location>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

#include "ex_actor/internal/alias.h"  // IWYU pragma: keep
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/reflect.h"

namespace ex_actor::internal {

template <typename Map, typename Key>
auto& MapAt(Map& map, const Key& key, std::source_location loc = std::source_location::current()) {
  auto it = map.find(key);
  if (it == map.end()) {
    throw std::out_of_range(
        fmt_lib::format("{}:{} in {}: map key '{}' not found", loc.file_name(), loc.line(), loc.function_name(), key));
  }
  return it->second;
}

// A sender that wraps two alternative senders in a variant, choosing at runtime.
// Used to type erase different senders without using ex::task, so the scheduler can be preserved.
template <class SenderA, class SenderB>
struct SenderVariant : ex::sender_t {
  std::variant<SenderA, SenderB> sender_variant;

  explicit SenderVariant(std::variant<SenderA, SenderB> sender) : sender_variant(std::move(sender)) {}

  using MergedSignatures = CompletionSignaturesUnion<ex::completion_signatures_of_t<SenderA, ex::env<>>,
                                                     ex::completion_signatures_of_t<SenderB, ex::env<>>>;

  template <class Self>
  static consteval auto get_completion_signatures() {
    return MergedSignatures {};
  }

  template <class Self, class Env>
  static consteval auto get_completion_signatures() {
    return MergedSignatures {};
  }

  template <ex::receiver Receiver>
  struct VariantOperation {
    using OpA = decltype(ex::connect(std::declval<SenderA>(), std::declval<Receiver>()));
    using OpB = decltype(ex::connect(std::declval<SenderB>(), std::declval<Receiver>()));

    // 0 = A active, 1 = B active
    unsigned char index;

    // Uses a plain union instead of std::variant to avoid requiring move-constructibility,
    // since stdexec operation states are immovable.
    union Storage {
      OpA op_a;
      OpB op_b;
      Storage() noexcept {}
      ~Storage() {}
    } storage;

    VariantOperation(std::integral_constant<unsigned char, 0> /*tag*/, SenderA&& sender, Receiver&& receiver)
        : index(0) {
      ::new (&storage.op_a) OpA(ex::connect(std::move(sender), std::move(receiver)));
    }
    VariantOperation(std::integral_constant<unsigned char, 1> /*tag*/, SenderB&& sender, Receiver&& receiver)
        : index(1) {
      ::new (&storage.op_b) OpB(ex::connect(std::move(sender), std::move(receiver)));
    }

    VariantOperation(VariantOperation&&) = delete;
    VariantOperation& operator=(VariantOperation&&) = delete;

    ~VariantOperation() {
      if (index == 0) {
        storage.op_a.~OpA();
      } else {
        storage.op_b.~OpB();
      }
    }

    void start() noexcept {
      if (index == 0) {
        ex::start(storage.op_a);
      } else {
        ex::start(storage.op_b);
      }
    }
  };

  template <ex::receiver Receiver>
  auto connect(Receiver receiver) && -> VariantOperation<Receiver> {
    if (sender_variant.index() == 0) {
      return {std::integral_constant<unsigned char, 0> {}, std::move(std::get<0>(sender_variant)), std::move(receiver)};
    }
    return {std::integral_constant<unsigned char, 1> {}, std::move(std::get<1>(sender_variant)), std::move(receiver)};
  }
};

}  // namespace ex_actor::internal

namespace ex_actor {
/**
 * @brief A std::execution style semaphore. support on_negative sender.
 */
class Semaphore {
 public:
  explicit Semaphore(int64_t initial_count) : count_(initial_count) {}

  int64_t Acquire(int64_t n) {
    auto prev = count_.fetch_sub(n, std::memory_order_release);
    if (prev <= n) {
      std::unique_lock lock(waiters_mu_);
      auto waiters = std::move(waiters_);
      lock.unlock();
      for (auto* waiter : waiters) {
        waiter->Complete();
      }
    }
    return prev - n;
  }

  int64_t Release(int64_t n) {
    auto prev = count_.fetch_add(n, std::memory_order_release);
    return prev + n;
  }

  int64_t CurrentValue() const { return count_.load(std::memory_order_acquire); }

  struct OnDrainedSender;

  /**
   * @brief Return a sender that will be completed when the semaphore is drained(value <= 0).
   */
  OnDrainedSender OnDrained() { return OnDrainedSender {.semaphore = this}; }

  struct TypeErasedOnDrainedOperation {
    virtual ~TypeErasedOnDrainedOperation() = default;
    virtual void Complete() = 0;
  };

  template <ex::receiver R>
  struct OnDrainedOperation : public TypeErasedOnDrainedOperation {
    OnDrainedOperation(Semaphore* semaphore, R receiver) : semaphore(semaphore), receiver(std::move(receiver)) {}
    Semaphore* semaphore;
    R receiver;

    void start() noexcept {
      // must get lock before checking count_, or if the count_ is drained before the waiter is queued but after
      // counter_ is checked, the waiter will never be completed
      std::scoped_lock lock(semaphore->waiters_mu_);
      if (semaphore->count_.load(std::memory_order_acquire) <= 0) {
        Complete();
      } else {
        semaphore->waiters_.push_back(this);
      }
    }

    void Complete() override { receiver.set_value(); }
  };

  struct OnDrainedSender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    Semaphore* semaphore;

    template <ex::receiver R>
    OnDrainedOperation<R> connect(R receiver) {
      return {semaphore, std::move(receiver)};
    }
  };

 private:
  mutable std::mutex waiters_mu_;
  std::vector<TypeErasedOnDrainedOperation*> waiters_;
  std::atomic_int64_t count_;
};

/// Wraps a sender so that its result is discarded and errors are logged.
/// By default, errors are fatal (calls std::terminate after logging).
/// Pass fatal_on_error=false to swallow errors instead.
/// The returned sender completes with `set_value_t()` (void) and never with `set_error_t`,
/// making it suitable for `ex::spawn` which requires exception-free senders.
///
/// Supports both direct-call and pipe syntax:
///   DiscardResult(sender)
///   sender | DiscardResult()
///   DiscardResult(sender, /*fatal_on_error=*/false)
///   sender | DiscardResult(/*fatal_on_error=*/false)
struct discard_result_closure_t : ex::sender_adaptor_closure<discard_result_closure_t> {
  bool fatal_on_error = true;

  template <ex::sender Sender>
  ex::sender auto operator()(Sender&& sender) const {
    auto log_and_swallow = [fatal = fatal_on_error](auto&& error) noexcept {
      const char* action = fatal ? "fatal" : "swallowed";
      if constexpr (std::is_same_v<std::decay_t<decltype(error)>, std::exception_ptr>) {
        try {
          std::rethrow_exception(error);
        } catch (const std::exception& e) {
          internal::log::Error("DiscardResult {} exception: {}", action, e);
        } catch (...) {
          internal::log::Error("DiscardResult {} unknown exception_ptr", action);
        }
      } else if constexpr (std::is_base_of_v<std::exception, std::decay_t<decltype(error)>>) {
        internal::log::Error("DiscardResult {} exception: {}", action, error);
      } else {
        internal::log::Error("DiscardResult {} error ({})", action, typeid(decltype(error)).name());
      }
      if (fatal) {
        std::terminate();
      }
    };
    return std::forward<Sender>(sender) | ex::then([](auto&&...) noexcept -> void {}) | ex::upon_error(log_and_swallow);
  }
};

struct discard_result_t {
  template <ex::sender Sender>
  ex::sender auto operator()(Sender&& sender, bool fatal_on_error) const {
    return discard_result_closure_t {.fatal_on_error = fatal_on_error}(std::forward<Sender>(sender));
  }

  template <ex::sender Sender>
  ex::sender auto operator()(Sender&& sender) const {
    return discard_result_closure_t {}(std::forward<Sender>(sender));
  }

  auto operator()(bool fatal_on_error) const -> discard_result_closure_t { return {.fatal_on_error = fatal_on_error}; }

  auto operator()() const -> discard_result_closure_t { return {}; }
};

inline constexpr discard_result_t DiscardResult {};

ex::sender auto StartsInline(ex::sender auto&& sender) {
  return ex::starts_on(ex::inline_scheduler {}, std::forward<decltype(sender)>(sender));
}

}  // namespace ex_actor

// Backward-compatibility alias — this namespace was removed in favor of ex_actor.
// Use ex_actor::Semaphore directly.
namespace ex_actor::util {
using Semaphore [[deprecated("Use ex_actor::Semaphore instead")]] = ex_actor::Semaphore;
}  // namespace ex_actor::util
