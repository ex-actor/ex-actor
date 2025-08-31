#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include <ex_actor/detail/reflect.h>
#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include "ex_actor/detail/util.h"

namespace ex_actor {

template <ex::scheduler Scheduler, reflect::SpecializationOf<ex::prop>... Props>
struct ActorConfig {
  size_t max_message_executed_per_activation = 100;
  std::tuple<Props...> std_exec_envs;
  Scheduler scheduler;
  std::optional<std::string> actor_name;
};

namespace detail {

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

struct TypeErasedActor {
  virtual ~TypeErasedActor() = default;
  virtual void PushMessage(ActorMessage* task) = 0;
  virtual void* GetUserClassAddress() = 0;
  virtual std::optional<std::string> GetActorName() const = 0;
};

// ---------------std::execution Scheduler Adaption-----------------

/**
 * @brief A std::execution scheduler that can be used to submit tasks on this actor. Note that it's different from
 * the scheduler passed to the Actor constructor, which is used to schedule the actor itself.
 */
struct StdExecSchedulerForActorMessageSubmission : public ex::scheduler_t {
  TypeErasedActor* actor;

  template <class Receiver>
  struct ActorMessageSubmissionOperation : ActorMessage {
    TypeErasedActor* actor;
    Receiver receiver;
    ActorMessageSubmissionOperation(TypeErasedActor* actor, Receiver receiver)
        : actor(actor), receiver(std::move(receiver)) {}
    void Execute() override { receiver.set_value(); }
    void start() noexcept {
      // According to the standard, the operation state will be alive until the task is executed,
      // so it's safe to push `this`.
      actor->PushMessage(this);
    }
    Type GetType() const override { return Type::kMethodCall; }
  };

  struct ActorMessageSubmissionSender : ex::sender_t {
    TypeErasedActor* actor;
    // NOLINTNEXTLINE(readability-identifier-naming)
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    struct Env {
      TypeErasedActor* actor;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> StdExecSchedulerForActorMessageSubmission {
        return {.actor = actor};
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
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  auto query(ex::get_forward_progress_guarantee_t) const noexcept -> ex::forward_progress_guarantee {
    return ex::forward_progress_guarantee::concurrent;
  }
};

// ---------------Actor Class---------------

template <class UserClass, reflect::SpecializationOf<ActorConfig> Config>
class Actor : public TypeErasedActor {
 public:
  template <typename... Args>
  explicit Actor(Config actor_config, Args&&... args)
      : actor_config_(std::move(actor_config)),
        user_class_instance_(std::make_unique<UserClass>(std::forward<Args>(args)...)) {}

  ~Actor() {
    auto destroy_msg = std::make_unique<DestroyMessage>();
    PushMessage(destroy_msg.get());
    ex::sync_wait(async_scope_.on_empty());
  }

  void PushMessage(ActorMessage* task) override {
    mailbox_.Push(task);
    TryActivate();
  }

  void* GetUserClassAddress() override { return user_class_instance_.get(); }

  std::optional<std::string> GetActorName() const override { return actor_config_.actor_name; }

 private:
  Config actor_config_;
  ThreadSafeQueue<ActorMessage*> mailbox_;
  std::unique_ptr<UserClass> user_class_instance_;
  exec::async_scope async_scope_;
  std::atomic_bool activated_ = false;

  // push self to the executor
  void TryActivate() {
    // CAS check, don't activate twice
    bool expect = false;
    bool changed = activated_.compare_exchange_strong(expect, /*desired=*/true, /*success=*/std::memory_order_acq_rel,
                                                      /*failure=*/std::memory_order_acquire);
    if (!changed) {
      return;
    }

    auto start_with_env = std::apply(
        [this](reflect::SpecializationOf<ex::prop> auto&&... props) {
          return (ex::schedule(actor_config_.scheduler) | ... | ex::write_env(props));
        },
        actor_config_.std_exec_envs);

    auto sender = std::move(start_with_env) | ex::then([this] { PullMailboxAndRun(); });
    async_scope_.spawn(std::move(sender));
  }

  void PullMailboxAndRun() {
    if (user_class_instance_ == nullptr) [[unlikely]] {
      // already destroyed
      return;
    }
    size_t message_executed = 0;
    while (!mailbox_.Empty()) {
      auto* msg = mailbox_.Pop();
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
    activated_.store(false, std::memory_order_release);
    if (!mailbox_.Empty()) {
      TryActivate();
    }
  }
};

}  // namespace detail
}  // namespace ex_actor