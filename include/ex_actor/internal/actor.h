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
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <rfl/Tuple.hpp>
#include <rfl/apply.hpp>
#include <stdexec/execution.hpp>

#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/reflect.h"
#include "ex_actor/internal/util.h"

namespace ex_actor::internal {
struct ActorMessage {
  enum class Type : uint8_t {
    kMethodCall = 0,
    kDestroy = 1,
  };
  virtual ~ActorMessage() = default;
  virtual void Execute() = 0;
  virtual Type GetType() const = 0;
};

struct DestroyMessage : ActorMessage {
  void Execute() override { throw std::runtime_error("DestroyMessage should not be executed"); }
  Type GetType() const override { return Type::kDestroy; }
};

class TypeErasedActor {
 public:
  explicit TypeErasedActor(ActorConfig actor_config) : actor_config_(std::move(actor_config)) {}
  virtual ~TypeErasedActor() = default;
  virtual void PushMessage(ActorMessage* task) = 0;
  virtual void* GetUserClassAddress() = 0;
  virtual exec::task<void> AsyncDestroy() = 0;

  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethod(Args... args);

  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethodUseTuple(std::tuple<Args...> args_tuple);

  const ActorConfig& GetActorConfig() const { return actor_config_; }

  virtual void PullMailboxAndRun() = 0;

 protected:
  ActorConfig actor_config_;
};

class TypeErasedActorScheduler {
 public:
  virtual ~TypeErasedActorScheduler() = default;
  virtual void Schedule(TypeErasedActor* actor, exec::async_scope& async_scope) = 0;
  virtual std::unique_ptr<TypeErasedActorScheduler> Clone() const = 0;

  virtual const void* GetUnderlyingSchedulerPtr() const = 0;
};

template <ex::scheduler Scheduler>
class AnyStdExecScheduler : public TypeErasedActorScheduler {
 public:
  explicit AnyStdExecScheduler(Scheduler scheduler) : scheduler_(std::move(scheduler)) {}
  void Schedule(TypeErasedActor* actor, exec::async_scope& async_scope) override {
    const auto& actor_config = actor->GetActorConfig();
    auto sender = ex::schedule(scheduler_) | ex::write_env(ex::prop {ex_actor::get_priority, actor_config.priority}) |
                  ex::write_env(ex::prop {ex_actor::get_scheduler_index, actor_config.scheduler_index}) |
                  ex::then([actor] { actor->PullMailboxAndRun(); });
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
struct StdExecSchedulerForActorMessageSubmission : public ex::scheduler_t {
  explicit StdExecSchedulerForActorMessageSubmission(TypeErasedActor* actor) : actor(actor) {}
  TypeErasedActor* actor;

  template <class Receiver>
  struct ActorMessageSubmissionOperation : ActorMessage {
    TypeErasedActor* actor;
    Receiver receiver;
    ActorMessageSubmissionOperation(TypeErasedActor* actor, Receiver receiver)
        : actor(actor), receiver(std::move(receiver)) {}
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
    Type GetType() const override { return Type::kMethodCall; }
  };

  struct ActorMessageSubmissionSender : ex::sender_t {
    TypeErasedActor* actor;
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;
    struct Env {
      TypeErasedActor* actor;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> StdExecSchedulerForActorMessageSubmission {
        return StdExecSchedulerForActorMessageSubmission(actor);
      }
    };
    auto get_env() const noexcept -> Env { return Env {.actor = actor}; }
    template <class Receiver>
    ActorMessageSubmissionOperation<Receiver> connect(Receiver receiver) noexcept {
      return {actor, std::move(receiver)};
    }
  };

  friend bool operator==(const StdExecSchedulerForActorMessageSubmission& lhs,
                         const StdExecSchedulerForActorMessageSubmission& rhs) noexcept {
    return lhs.actor == rhs.actor;
  }
  ActorMessageSubmissionSender schedule() const noexcept { return {.actor = actor}; }
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
    pending_to_be_destroyed_.store(true, std::memory_order_release);
    pending_message_count_.fetch_add(1, std::memory_order_release);
    TryActivate();
    co_await async_scope_.on_empty();
  }

  void PushMessage(ActorMessage* task) override {
    mailbox_.Push(task);
    pending_message_count_.fetch_add(1, std::memory_order_release);
    TryActivate();
  }

  void* GetUserClassAddress() override { return user_class_instance_.get(); }

 private:
  std::unique_ptr<TypeErasedActorScheduler> scheduler_;
  util::LinearizableUnboundedQueue<ActorMessage*> mailbox_;
  std::atomic_size_t pending_message_count_ = 0;
  std::unique_ptr<UserClass> user_class_instance_;
  exec::async_scope async_scope_;
  std::atomic_bool activated_ = false;
  std::atomic_bool pending_to_be_destroyed_ = false;

  // push self to the executor
  void TryActivate() {
    // CAS check, don't activate twice
    bool expect = false;
    bool changed = activated_.compare_exchange_strong(expect, /*desired=*/true, /*success=*/std::memory_order_acq_rel,
                                                      /*failure=*/std::memory_order_acquire);
    if (!changed) {
      return;
    }
    scheduler_->Schedule(this, async_scope_);
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

  std::string Description() {
    return fmt_lib::format("Actor {}(type:{},name:{})", (void*)this, typeid(UserClass).name(),
                           actor_config_.actor_name.value_or("null"));
  }
};  // class Actor

template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethod(Args... args) {
  return CallActorMethodUseTuple<kMethod>(std::make_tuple(std::move(args)...));
}

template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethodUseTuple(std::tuple<Args...> args_tuple) {
  using Sig = reflect::Signature<decltype(kMethod)>;
  using ReturnType = Sig::ReturnType;
  using UserClass = Sig::ClassType;
  constexpr bool kIsNested = ex::sender<ReturnType>;
  auto start = ex::schedule(StdExecSchedulerForActorMessageSubmission(this));

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
