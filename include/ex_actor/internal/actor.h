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

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

#include <rfl/Tuple.hpp>
#include <rfl/apply.hpp>
#include <stdexec/execution.hpp>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/container.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/scheduler.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {
struct ActorMessage {
  virtual ~ActorMessage() = default;
  virtual void Execute() = 0;
};

class TypeErasedActor {
 public:
  explicit TypeErasedActor(ActorConfig actor_config) : actor_config_(std::move(actor_config)) {}
  virtual ~TypeErasedActor() = default;
  virtual void PushMessage(ActorMessage* task, size_t mailbox_index) = 0;
  virtual ex::task<void> AsyncDestroy() = 0;
  virtual uint64_t GetActorTypeHash() const = 0;
  virtual size_t GetPendingMessageCount() const = 0;

  const ActorConfig& GetActorConfig() const { return actor_config_; }
  void* GetUserClassInstanceAddress() const { return cached_user_class_instance_address_; }

  virtual void PullMailboxAndRun() = 0;

 protected:
  void* cached_user_class_instance_address_ = nullptr;
  ActorConfig actor_config_;
};

class TypeErasedActorScheduler {
 public:
  virtual ~TypeErasedActorScheduler() = default;
  virtual void Schedule(TypeErasedActor* actor, ex::simple_counting_scope::token scope_token) = 0;
  virtual std::unique_ptr<TypeErasedActorScheduler> Clone() const = 0;

  virtual const void* GetUnderlyingSchedulerPtr() const = 0;
};

template <ex::scheduler Scheduler>
class AnyStdExecScheduler : public TypeErasedActorScheduler {
 public:
  explicit AnyStdExecScheduler(Scheduler scheduler) : scheduler_(std::move(scheduler)) {}
  void Schedule(TypeErasedActor* actor, ex::simple_counting_scope::token scope_token) override {
    const auto& actor_config = actor->GetActorConfig();
    auto sender = ex::schedule(scheduler_) | ex::write_env(ex::prop {ex_actor::get_priority, actor_config.priority}) |
                  ex::write_env(ex::prop {ex_actor::get_scheduler_index, actor_config.scheduler_index}) |
                  ex::then([actor] { actor->PullMailboxAndRun(); });
    ex::spawn(std::move(sender) | DiscardResult(), scope_token);
  }

  std::unique_ptr<TypeErasedActorScheduler> Clone() const override {
    return std::make_unique<AnyStdExecScheduler<Scheduler>>(scheduler_);
  }

  const void* GetUnderlyingSchedulerPtr() const override { return &scheduler_; }

 private:
  Scheduler scheduler_;
};

// ---------------std::execution Scheduler Adaption-----------------

/**
 * @brief A std::execution scheduler that can be used to submit tasks on this actor. Note that it's different from
 * the scheduler passed to the Actor constructor, which is used to schedule the actor itself.
 */
struct StdExecSchedulerForActorMessageSubmission : public ex::scheduler_t {
  explicit StdExecSchedulerForActorMessageSubmission(TypeErasedActor* actor, size_t mailbox_index = 0)
      : actor(actor), mailbox_index(mailbox_index) {}
  TypeErasedActor* actor;
  size_t mailbox_index;

  template <class Receiver>
  struct ActorMessageSubmissionOperation : ActorMessage {
    TypeErasedActor* actor;
    Receiver receiver;
    size_t mailbox_index;
    ActorMessageSubmissionOperation(TypeErasedActor* actor, Receiver receiver, size_t mailbox_index)
        : actor(actor), receiver(std::move(receiver)), mailbox_index(mailbox_index) {}
    void Execute() override {
      auto stoken = ex::get_stop_token(ex::get_env(receiver));
      if constexpr (ex::unstoppable_token<decltype(stoken)>) {
        receiver.set_value();
      } else {
        if (stoken.stop_requested()) {
          receiver.set_stopped();
        } else {
          receiver.set_value();
        }
      }
    }
    void start() noexcept {
      // According to the standard, the operation state will be alive until the task is executed,
      // so it's safe to push `this`.
      actor->PushMessage(this, mailbox_index);
    }
  };

  struct ActorMessageSubmissionSender : ex::sender_t, StoppableSchedulerCompletionSignatures {
    using StoppableSchedulerCompletionSignatures::get_completion_signatures;
    TypeErasedActor* actor;
    size_t mailbox_index;

    struct Env {
      TypeErasedActor* actor;
      size_t mailbox_index;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> StdExecSchedulerForActorMessageSubmission {
        return StdExecSchedulerForActorMessageSubmission(actor, mailbox_index);
      }
    };
    auto get_env() const noexcept -> Env { return Env {.actor = actor, .mailbox_index = mailbox_index}; }
    template <class Receiver>
    ActorMessageSubmissionOperation<Receiver> connect(Receiver receiver) noexcept {
      return {actor, std::move(receiver), mailbox_index};
    }
  };

  friend bool operator==(const StdExecSchedulerForActorMessageSubmission& lhs,
                         const StdExecSchedulerForActorMessageSubmission& rhs) noexcept {
    return lhs.actor == rhs.actor && lhs.mailbox_index == rhs.mailbox_index;
  }
  ActorMessageSubmissionSender schedule() const noexcept { return {.actor = actor, .mailbox_index = mailbox_index}; }
  auto query(ex::get_forward_progress_guarantee_t) const noexcept -> ex::forward_progress_guarantee {
    return ex::forward_progress_guarantee::concurrent;
  }
};

// ---------------Mailbox--------------------

// A set of mailboxes that supports a heterogeneous set of per-mailbox queue types. All
// queues are inlined (via std::variant) to avoid virtual functions, and stored directly
// inside a FixedCapacityBuffer so there is no per-queue heap allocation or pointer chase
// in Push/TryPop.
class MailboxSet {
 public:
  explicit MailboxSet(const std::vector<MailboxConfig>& configs) : mailboxes_(configs.empty() ? 1 : configs.size()) {
    if (configs.empty()) {
      // Per ActorConfig::mailbox_configs contract: empty means one default UnboundedThreadSafeMailbox.
      mailboxes_.EmplaceBack(std::in_place_type<LinearizableUnboundedMpscQueue<ActorMessage*>>);
      return;
    }
    for (const auto& cfg : configs) {
      std::visit(
          [&]<class T>(const T&) {
            if constexpr (std::is_same_v<T, UnboundedThreadSafeMailbox>) {
              mailboxes_.EmplaceBack(std::in_place_type<LinearizableUnboundedMpscQueue<ActorMessage*>>);
            } else if constexpr (std::is_same_v<T, UnsafeOneSlotMailbox>) {
              mailboxes_.EmplaceBack(std::in_place_type<OneSlotUnsafeQueue<ActorMessage*>>);
            }
          },
          cfg);
    }
  }

  void Push(size_t mailbox_index, ActorMessage* message) {
    std::visit(
        [&](auto& queue) {
          bool ok = queue.Push(message);
          EXA_THROW_CHECK(ok) << "MailboxSet: mailbox " << mailbox_index << " is full";
        },
        mailboxes_[mailbox_index]);
  }

  std::optional<ActorMessage*> TryPop(size_t mailbox_index) {
    return std::visit([&](auto& queue) { return queue.TryPop(); }, mailboxes_[mailbox_index]);
  }

  size_t MailboxCount() const { return mailboxes_.Size(); }

 private:
  // The MPSC queue's move/copy are deleted, which makes the variant non-movable. That's fine
  // here because FixedCapacityBuffer constructs elements in place and never relocates them.
  using MailboxStorage = std::variant<LinearizableUnboundedMpscQueue<ActorMessage*>, OneSlotUnsafeQueue<ActorMessage*>>;

  FixedCapacityBuffer<MailboxStorage> mailboxes_;
};

// ---------------Actor Class---------------

template <class UserClass, auto kCreateFn = nullptr>
class Actor : public TypeErasedActor {
 public:
  template <typename... Args>
  explicit Actor(std::unique_ptr<TypeErasedActorScheduler> scheduler, ActorConfig actor_config, Args... args)
      : TypeErasedActor(std::move(actor_config)),
        scheduler_(std::move(scheduler)),
        mailbox_(actor_config_.mailbox_configs) {
    if constexpr (kCreateFn != nullptr) {
      using ReturnType = FnReturnType<kCreateFn>;
      static_assert(
          std::is_same_v<ReturnType, UserClass>,
          "The return type of kCreateFn must match the Actor's UserClass template parameter to avoid slicing.");

      // Use `new` directly so the prvalue returned by kCreateFn is used to
      // initialize the heap object, benefiting from guaranteed copy elision
      // (C++17 [dcl.init]/17.6.1) and not requiring UserClass to be movable.
      user_class_instance_.reset(new UserClass(kCreateFn(std::move(args)...)));
    } else {
      user_class_instance_ = std::make_unique<UserClass>(std::move(args)...);
    }
    cached_user_class_instance_address_ = user_class_instance_.get();
  }

  template <typename... Args>
  static std::unique_ptr<TypeErasedActor> CreateUseArgTuple(std::unique_ptr<TypeErasedActorScheduler> scheduler,
                                                            ActorConfig actor_config, std::tuple<Args...> arg_tuple) {
    return std::apply(
        [scheduler = std::move(scheduler), actor_config = std::move(actor_config)](auto&&... args) mutable {
          return std::make_unique<Actor<UserClass, kCreateFn>>(std::move(scheduler), std::move(actor_config),
                                                               std::move(args)...);
        },
        std::move(arg_tuple));
  }

  ~Actor() override = default;

  uint64_t GetActorTypeHash() const override { return FnvHash(GetTypeName<UserClass>()); }

  /// Async destroy the actor, if there are still messages in the mailbox, they might not be processed.
  ex::task<void> AsyncDestroy() override {
    bool expected = false;
    bool changed = pending_to_be_destroyed_.compare_exchange_strong(expected, /*desired=*/true,
                                                                    /*success=*/std::memory_order_release,
                                                                    /*failure=*/std::memory_order_acquire);
    if (!changed) {
      co_return;
    }
    pending_message_count_.fetch_add(1, std::memory_order_release);
    if (ShouldActivate(MailboxPushEvent {.mailbox_index = 0, .is_destroy_signal = true})) {
      TryActivate();
    }
    co_await async_scope_.join();
  }

  void PushMessage(ActorMessage* task, size_t mailbox_index) override {
    mailbox_.Push(mailbox_index, task);
    pending_message_count_.fetch_add(1, std::memory_order_release);
    if (ShouldActivate(MailboxPushEvent {.mailbox_index = mailbox_index, .is_destroy_signal = false})) {
      TryActivate();
    }
  }

  size_t GetPendingMessageCount() const override { return pending_message_count_.load(std::memory_order_acquire); }

 private:
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  MailboxSet mailbox_;
  std::atomic_size_t pending_message_count_ = 0;
  std::unique_ptr<UserClass> user_class_instance_;
  ex::simple_counting_scope async_scope_;
  std::atomic_bool activated_ = false;
  std::atomic_bool pending_to_be_destroyed_ = false;

  // Compile-time dispatched hook. When the user actor class defines
  // `bool ExActorShouldActivate(MailboxPushEvent)`, the return value of that method
  // gates whether the actor is activated for this push. Otherwise, defaults to true.
  bool ShouldActivate(MailboxPushEvent event) {
    if constexpr (HasShouldActivateHook<UserClass>()) {
      return user_class_instance_->ExActorShouldActivate(event);
    } else {
      return true;
    }
  }

  // push self to the executor
  void TryActivate() {
    // CAS check, don't activate twice
    bool expect = false;
    bool changed = activated_.compare_exchange_strong(expect, /*desired=*/true, /*success=*/std::memory_order_acq_rel,
                                                      /*failure=*/std::memory_order_acquire);
    if (!changed) {
      return;
    }
    scheduler_->Schedule(this, async_scope_.get_token());
  }

  void PullMailboxAndRun() override {
    if (user_class_instance_ == nullptr) [[unlikely]] {
      // already destroyed
      size_t remaining = pending_message_count_.load(std::memory_order_acquire);
      log::Warn("{} is already destroyed, but triggered again, it has {} messages remaining", Description(), remaining);
      return;
    }

    if (pending_to_be_destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
      user_class_instance_.reset();
      activated_.store(false, std::memory_order_release);
      size_t remaining = pending_message_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining > 0) {
        log::Warn("{} is destroyed but still has {} messages remaining", Description(), remaining);
      }
      return;
    }

    size_t message_executed = 0;
    size_t message_execution_limit = std::min(actor_config_.max_message_executed_per_activation,
                                              pending_message_count_.load(std::memory_order_acquire));
    auto mailbox_number = mailbox_.MailboxCount();
    // Walk each mailbox once, fully draining it before moving on to the next, subject
    // to the per-activation message limit. Messages pushed after the walk reaches a
    // later mailbox stay queued for the next activation.
    bool limit_reached = false;
    for (size_t mailbox_index = 0; mailbox_index < mailbox_number && !limit_reached; ++mailbox_index) {
      while (auto optional_msg = mailbox_.TryPop(mailbox_index)) {
        optional_msg.value()->Execute();
        message_executed++;
        if (message_executed >= message_execution_limit) [[unlikely]] {
          limit_reached = true;
          break;
        }
      }
    }

    pending_message_count_.fetch_sub(message_executed, std::memory_order_release);

    // use seq_cst to prevent reordering activated_.store() and pending_message_count_.load()
    // or the actor might not be activated correctly
    activated_.store(false, std::memory_order_seq_cst);

    if (pending_message_count_.load(std::memory_order_acquire) > 0) {
      TryActivate();
    }
  }

  std::string Description() {
    return fmt_lib::format("Actor {}(type:{},name:{})", (void*)this, typeid(UserClass).name(),
                           actor_config_.actor_name.value_or("null"));
  }
};  // class Actor

template <auto kMethod, class PtrClass, class... Args>
  requires(std::is_invocable_v<decltype(kMethod), PtrClass*, Args...>)
ex::sender auto CallActorMethodUseTuple(TypeErasedActor* actor, PtrClass* adjusted_ptr, std::tuple<Args...> args_tuple,
                                        size_t mailbox_index = 0) {
  using Sig = Signature<decltype(kMethod)>;
  using ReturnType = Sig::ReturnType;
  constexpr bool kIsNested = ex::sender<ReturnType>;
  auto start = ex::schedule(StdExecSchedulerForActorMessageSubmission(actor, mailbox_index));

  // Convert caller-provided args to the method's decayed parameter types up-front, so any implicit conversions
  // (e.g. const char* -> std::string) materialize into the stored tuple rather than temporaries that die at the end
  // of the invocation expression. This keeps reference parameters of coroutine methods bound to objects owned by the
  // operation state.
  using StoredTupleType = typename Sig::DecayedArgsTupleType;
  StoredTupleType stored_args_tuple = std::make_from_tuple<StoredTupleType>(std::move(args_tuple));

  if constexpr (kIsNested) {
    return std::move(start) | ex::let_value([adjusted_ptr, args_tuple = std::move(stored_args_tuple)]() mutable {
             return std::apply([adjusted_ptr](auto&&... args) { return (adjusted_ptr->*kMethod)(std::move(args)...); },
                               std::move(args_tuple));
           });
  } else {
    return std::move(start) | ex::then([adjusted_ptr, args_tuple = std::move(stored_args_tuple)]() mutable {
             return std::apply([adjusted_ptr](auto&&... args) { return (adjusted_ptr->*kMethod)(std::move(args)...); },
                               std::move(args_tuple));
           });
  }
}
}  // namespace ex_actor::internal
