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

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <rfl/Tuple.hpp>
#include <rfl/apply.hpp>
#include <stdexec/execution.hpp>

#include "ex_actor/internal/common_structs.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {
// ------------type erased actor&message------------------

class TypeErasedActor;

/**
 * @brief Common context for actor message submission, holding actor pointer and message metadata.
 */
struct ActorMessageContext {
  TypeErasedActor* actor = nullptr;
  std::optional<size_t> unsafe_message_slot_index;

  friend bool operator==(const ActorMessageContext& lhs, const ActorMessageContext& rhs) noexcept {
    return lhs.actor == rhs.actor && lhs.unsafe_message_slot_index == rhs.unsafe_message_slot_index;
  }
};

struct ActorMessage : ActorMessageContext {
  explicit ActorMessage(ActorMessageContext context) : ActorMessageContext(context) {}

  virtual ~ActorMessage() = default;
  virtual void Execute() = 0;
};

class TypeErasedActor {
 public:
  explicit TypeErasedActor(ActorConfig actor_config) : actor_config_(std::move(actor_config)) {}
  virtual ~TypeErasedActor() = default;
  virtual void PushMessage(ActorMessage* task) = 0;
  virtual void* GetUserClassAddress() = 0;
  virtual exec::task<void> AsyncDestroy() = 0;

  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethod(std::optional<size_t> unsafe_message_slot_index, Args... args);

  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethodUseTuple(std::optional<size_t> unsafe_message_slot_index,
                                          std::tuple<Args...> args_tuple);

  const ActorConfig& GetActorConfig() const { return actor_config_; }

  virtual void PullMailboxAndRun() = 0;
  virtual void PullUnsafeMessageSlotsAndRun() = 0;

 protected:
  ActorConfig actor_config_;
};

class TypeErasedActorScheduler {
 public:
  virtual ~TypeErasedActorScheduler() = default;
  virtual void Schedule(TypeErasedActor* actor, exec::async_scope& async_scope,
                        bool is_unsafe_message_slot_activation) = 0;
  virtual std::unique_ptr<TypeErasedActorScheduler> Clone() const = 0;

  virtual const void* GetUnderlyingSchedulerPtr() const = 0;
};

template <ex::scheduler Scheduler>
class AnyStdExecScheduler : public TypeErasedActorScheduler {
 public:
  explicit AnyStdExecScheduler(Scheduler scheduler) : scheduler_(std::move(scheduler)) {}
  void Schedule(TypeErasedActor* actor, exec::async_scope& async_scope,
                bool is_unsafe_message_slot_activation) override {
    const auto& actor_config = actor->GetActorConfig();
    auto sender = ex::schedule(scheduler_) | ex::write_env(ex::prop {ex_actor::get_priority, actor_config.priority}) |
                  ex::write_env(ex::prop {ex_actor::get_scheduler_index, actor_config.scheduler_index}) |
                  ex::then([actor, is_unsafe_message_slot_activation] {
                    if (is_unsafe_message_slot_activation) {
                      actor->PullUnsafeMessageSlotsAndRun();
                    } else {
                      actor->PullMailboxAndRun();
                    }
                  });
    async_scope.spawn(std::move(sender));
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
struct StdExecSchedulerForActorMessageSubmission : public ex::scheduler_t, ActorMessageContext {
  explicit StdExecSchedulerForActorMessageSubmission(ActorMessageContext context) : ActorMessageContext(context) {}

  template <class Receiver>
  struct ActorMessageSubmissionOperation : ActorMessage {
    Receiver receiver;
    ActorMessageSubmissionOperation(ActorMessageContext context, Receiver receiver)
        : ActorMessage(context), receiver(std::move(receiver)) {}
    void Execute() override {
      auto stoken = stdexec::get_stop_token(stdexec::get_env(receiver));
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
      actor->PushMessage(this);
    }
  };

  struct ActorMessageSubmissionSender : ex::sender_t, ActorMessageContext {
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;
    struct Env : ActorMessageContext {
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> StdExecSchedulerForActorMessageSubmission {
        return StdExecSchedulerForActorMessageSubmission(*this);
      }
    };
    auto get_env() const noexcept -> Env { return Env(*this); }
    template <class Receiver>
    ActorMessageSubmissionOperation<Receiver> connect(Receiver receiver) noexcept {
      return {ActorMessageContext {*this}, std::move(receiver)};
    }
  };

  friend bool operator==(const StdExecSchedulerForActorMessageSubmission& lhs,
                         const StdExecSchedulerForActorMessageSubmission& rhs) noexcept {
    return static_cast<const ActorMessageContext&>(lhs) == static_cast<const ActorMessageContext&>(rhs);
  }
  ActorMessageSubmissionSender schedule() const noexcept {
    return ActorMessageSubmissionSender {/*sender_t*/ {}, ActorMessageContext {*this}};
  }
  auto query(ex::get_forward_progress_guarantee_t) const noexcept -> ex::forward_progress_guarantee {
    return ex::forward_progress_guarantee::concurrent;
  }
};

// ---------------Actor Class---------------

template <class UserClass, auto kCreateFn = nullptr>
class Actor : public TypeErasedActor {
 public:
  template <typename... Args>
  explicit Actor(std::unique_ptr<TypeErasedActorScheduler> scheduler, ActorConfig actor_config, Args... args)
      : TypeErasedActor(std::move(actor_config)), scheduler_(std::move(scheduler)) {
    if constexpr (kCreateFn != nullptr) {
      user_class_instance_ = std::make_unique<UserClass>(kCreateFn(std::move(args)...));
    } else {
      user_class_instance_ = std::make_unique<UserClass>(std::move(args)...);
    }
    unsafe_message_slots_.resize(actor_config_.unsafe_message_slots);
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

  /// Async destroy the actor, if there are still messages in the mailbox, they might not be processed.
  exec::task<void> AsyncDestroy() override {
    bool expected = false;
    bool changed = pending_to_be_destroyed_.compare_exchange_strong(expected, true, std::memory_order_release,
                                                                    std::memory_order_acquire);
    if (!changed) {
      co_return;
    }
    pending_message_count_.fetch_add(1, std::memory_order_release);
    TryActivate();
    co_await async_scope_.on_empty();
  }

  void PushMessage(ActorMessage* task) override {
    // normal mailbox message
    if (!task->unsafe_message_slot_index.has_value()) {
      mailbox_.Push(task);
      pending_message_count_.fetch_add(1, std::memory_order_release);
      TryActivate();
      return;
    }

    // unsafe message slot
    unsafe_message_slots_.at(task->unsafe_message_slot_index.value()) = task;
    if constexpr (reflect::HasOnUnsafeMessageSlotFilledHook<UserClass>) {
      bool should_activate =
          user_class_instance_->ExActorOnUnsafeMessageSlotFilled(task->unsafe_message_slot_index.value());
      if (should_activate) {
        scheduler_->Schedule(this, async_scope_, /*is_unsafe_message_slot_activation=*/true);
      }
    } else {
      EXA_THROW << "User class " << typeid(UserClass).name() << " does not have OnUnsafeMessageSlotFilled hook";
    }
  }

  void* GetUserClassAddress() override { return user_class_instance_.get(); }

 private:
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  util::LinearizableUnboundedMpscQueue<ActorMessage*> mailbox_;
  std::vector<ActorMessage*> unsafe_message_slots_;
  std::atomic_size_t pending_message_count_ = 0;
  std::unique_ptr<UserClass> user_class_instance_;
  exec::async_scope async_scope_;
  std::atomic_bool activated_ = false;
  std::atomic_bool pending_to_be_destroyed_ = false;

  void TryActivate() {
    // Auto activate mode, do a CAS check, don't activate twice
    bool expect = false;
    bool changed = activated_.compare_exchange_strong(expect, /*desired=*/true, /*success=*/std::memory_order_acq_rel,
                                                      /*failure=*/std::memory_order_acquire);
    if (!changed) {
      return;
    }
    scheduler_->Schedule(this, async_scope_, /*is_unsafe_message_slot_activation=*/false);
  }

  void PullMailboxAndRun() override {
    if (user_class_instance_ == nullptr) [[unlikely]] {
      // already destroyed
      size_t remaining = pending_message_count_.load(std::memory_order_acquire);
      logging::Warn("{} is already destroyed, but triggered again, it has {} messages remaining", Description(),
                    remaining);
      return;
    }

    if (pending_to_be_destroyed_.load(std::memory_order_acquire)) [[unlikely]] {
      user_class_instance_.reset();
      activated_.store(false, std::memory_order_release);
      size_t remaining = pending_message_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
      if (remaining > 0) {
        logging::Warn("{} is destroyed but still has {} messages remaining", Description(), remaining);
      }
      return;
    }

    size_t message_executed = 0;
    while (auto optional_msg = mailbox_.TryPop()) {
      optional_msg.value()->Execute();
      message_executed++;
      if (message_executed >= actor_config_.max_message_executed_per_activation) [[unlikely]] {
        break;
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

  void PullUnsafeMessageSlotsAndRun() override {
    for (auto& task : unsafe_message_slots_) {
      if (task != nullptr) [[likely]] {
        task->Execute();
        task = nullptr;
      }
    }
  }

  std::string Description() {
    return fmt_lib::format("Actor {}(type:{},name:{})", (void*)this, typeid(UserClass).name(),
                           actor_config_.actor_name.value_or("null"));
  }
};  // class Actor

template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethod(std::optional<size_t> unsafe_message_slot_index, Args... args) {
  return CallActorMethodUseTuple<kMethod>(unsafe_message_slot_index, std::make_tuple(std::move(args)...));
}

template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethodUseTuple(std::optional<size_t> unsafe_message_slot_index,
                                                         std::tuple<Args...> args_tuple) {
  using Sig = reflect::Signature<decltype(kMethod)>;
  using ReturnType = Sig::ReturnType;
  using UserClass = Sig::ClassType;
  constexpr bool kIsNested = ex::sender<ReturnType>;
  auto start = ex::schedule(StdExecSchedulerForActorMessageSubmission(
      ActorMessageContext {.actor = this, .unsafe_message_slot_index = unsafe_message_slot_index}));

  auto* user_class_instance = static_cast<UserClass*>(GetUserClassAddress());

  if constexpr (kIsNested) {
    return std::move(start) | ex::let_value([user_class_instance, args_tuple = std::move(args_tuple)]() mutable {
             return std::apply(
                 [user_class_instance](auto&&... args) { return (user_class_instance->*kMethod)(std::move(args)...); },
                 std::move(args_tuple));
           });
  } else {
    return std::move(start) | ex::then([user_class_instance, args_tuple = std::move(args_tuple)]() mutable {
             return std::apply(
                 [user_class_instance](auto&&... args) { return (user_class_instance->*kMethod)(std::move(args)...); },
                 std::move(args_tuple));
           });
  }
}
}  // namespace ex_actor::internal