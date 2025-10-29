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

  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethod(Args&&... args);

  template <auto kMethod, class... Args>
  ex::sender auto CallActorMethodUseTuple(rfl::Tuple<Args...> args_tuple);

  const ActorConfig& GetActorConfig() const { return actor_config_; }

 protected:
  ActorConfig actor_config_;
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

template <class UserClass, ex::scheduler Scheduler, bool kUseStaticCreateFn = false>
class Actor : public TypeErasedActor {
 public:
  template <typename... Args>
  explicit Actor(Scheduler scheduler, ActorConfig actor_config, Args&&... args)
      : TypeErasedActor(std::move(actor_config)), scheduler_(std::move(scheduler)) {
    if constexpr (kUseStaticCreateFn) {
      user_class_instance_ = std::make_unique<UserClass>(UserClass::Create(std::forward<Args>(args)...));
    } else {
      user_class_instance_ = std::make_unique<UserClass>(std::forward<Args>(args)...);
    }
  }

  template <typename... Args>
  static std::unique_ptr<TypeErasedActor> CreateUseArgTuple(Scheduler scheduler, ActorConfig actor_config,
                                                            rfl::Tuple<Args...> arg_tuple) {
    return rfl::apply(
        [scheduler = std::move(scheduler), actor_config = std::move(actor_config)](auto&&... args) {
          return std::make_unique<Actor<UserClass, Scheduler, /*kUseStaticCreateFn=*/true>>(
              std::move(scheduler), std::move(actor_config), std::move(args)...);
        },
        std::move(arg_tuple));
  }

  ~Actor() override {
    if (destroy_message_pushed_.load(std::memory_order_acquire)) {
      ex::sync_wait(async_scope_.on_empty());
      return;
    }
    auto destroy_msg = std::make_unique<DestroyMessage>();
    PushMessage(destroy_msg.get());
    ex::sync_wait(async_scope_.on_empty());
  }

  void PushMessage(ActorMessage* task) override {
    if (task->GetType() == ActorMessage::Type::kDestroy) [[unlikely]] {
      destroy_message_pushed_.store(true, std::memory_order_release);
    }
    mailbox_.Push(task);
    pending_message_count_.fetch_add(1, std::memory_order_release);
    TryActivate();
  }

  void* GetUserClassAddress() override { return user_class_instance_.get(); }

 private:
  Scheduler scheduler_;
  util::LinearizableUnboundedQueue<ActorMessage*> mailbox_;
  std::atomic_size_t pending_message_count_ = 0;
  std::unique_ptr<UserClass> user_class_instance_;
  exec::async_scope async_scope_;
  std::atomic_bool activated_ = false;
  std::atomic_bool destroy_message_pushed_ = false;

  // push self to the executor
  void TryActivate() {
    // CAS check, don't activate twice
    bool expect = false;
    bool changed = activated_.compare_exchange_strong(expect, /*desired=*/true, /*success=*/std::memory_order_acq_rel,
                                                      /*failure=*/std::memory_order_acquire);
    if (!changed) {
      return;
    }

    auto sender = ex::schedule(scheduler_) |
                  ex::write_env(ex::prop {ex_actor::get_priority, GetActorConfig().priority}) |
                  ex::write_env(ex::prop {ex_actor::get_scheduler_index, GetActorConfig().scheduler_index}) |
                  ex::then([this] { PullMailboxAndRun(); });
    async_scope_.spawn(std::move(sender));
  }

  void PullMailboxAndRun() {
    if (user_class_instance_ == nullptr) [[unlikely]] {
      // already destroyed
      return;
    }

    size_t message_executed = 0;
    while (auto optional_msg = mailbox_.TryPop()) {
      auto* msg = optional_msg.value();
      if (msg->GetType() == ActorMessage::Type::kDestroy) [[unlikely]] {
        user_class_instance_.reset();
        return;
      }
      msg->Execute();
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
};  // class Actor

template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethod(Args&&... args) {
  return CallActorMethodUseTuple<kMethod>(rfl::make_tuple(std::forward<Args>(args)...));
}

template <auto kMethod, class... Args>
ex::sender auto TypeErasedActor::CallActorMethodUseTuple(rfl::Tuple<Args...> args_tuple) {
  using Sig = reflect::Signature<decltype(kMethod)>;
  using ReturnType = Sig::ReturnType;
  using UserClass = Sig::ClassType;
  constexpr bool kIsNested = ex::sender<ReturnType>;
  auto start = ex::schedule(StdExecSchedulerForActorMessageSubmission(this));

  auto* user_class_instance = static_cast<UserClass*>(GetUserClassAddress());

  if constexpr (kIsNested) {
    return std::move(start) | ex::let_value([user_class_instance, args_tuple = std::move(args_tuple)]() mutable {
             return rfl::apply(
                 [user_class_instance](auto&&... args) { return (user_class_instance->*kMethod)(std::move(args)...); },
                 std::move(args_tuple));
           });
  } else {
    return std::move(start) | ex::then([user_class_instance, args_tuple = std::move(args_tuple)]() mutable {
             return rfl::apply(
                 [user_class_instance](auto&&... args) { return (user_class_instance->*kMethod)(std::move(args)...); },
                 std::move(args_tuple));
           });
  }
}
}  // namespace ex_actor::internal
