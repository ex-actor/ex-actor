#pragma once

#include <thread>

#include <exec/static_thread_pool.hpp>

#include "ex_actor/3rd_lib/moody_camel_queue/blockingconcurrentqueue.h"
#include "ex_actor/internal/actor_config.h"
#include "ex_actor/internal/util.h"

namespace ex_actor {

template <template <class> class Queue>
class WorkSharingThreadPoolBase {
 public:
  explicit WorkSharingThreadPoolBase(size_t thread_count, bool start_workers_immediately = true)
      : thread_count_(thread_count) {
    if (start_workers_immediately) {
      StartWorkers();
    }
  }

  void StartWorkers() {
    for (size_t i = 0; i < thread_count_; ++i) {
      workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
    }
  }

  struct TypeEasedOperation {
    virtual ~TypeEasedOperation() = default;
    virtual void Execute() = 0;
  };

  template <ex::receiver R>
  struct Operation : TypeEasedOperation {
    Operation(R receiver, WorkSharingThreadPoolBase* thread_pool)
        : receiver(std::move(receiver)), thread_pool(thread_pool) {}
    R receiver;
    WorkSharingThreadPoolBase* thread_pool;
    void Execute() override {
      auto env = stdexec::get_env(receiver);
      auto stoken = stdexec::get_stop_token(env);
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
      uint32_t priority = UINT32_MAX;
      if constexpr (std::is_same_v<Queue<TypeEasedOperation*>,
                                   internal::util::UnboundedBlockingPriorityQueue<TypeEasedOperation*>>) {
        auto env = stdexec::get_env(receiver);
        std::optional std_exec_envs = ex_actor::get_std_exec_env(env);

        if (std_exec_envs.has_value()) {
          auto iter = std_exec_envs->find("priority");
          if (iter != std_exec_envs->end()) {
            priority = std::stoi(iter->second);
          }
        }
      }
      thread_pool->EnqueueOperation(this, priority);
    }
  };

  struct Scheduler;

  struct Sender : ex::sender_t {
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;
    WorkSharingThreadPoolBase* thread_pool;
    struct Env {
      WorkSharingThreadPoolBase* thread_pool;
      template <class CPO>
      auto query(ex::get_completion_scheduler_t<CPO>) const noexcept -> Scheduler {
        return {.thread_pool = thread_pool};
      }
    };
    auto get_env() const noexcept -> Env { return Env {.thread_pool = thread_pool}; }
    template <ex::receiver R>
    Operation<R> connect(R receiver) {
      return {std::move(receiver), thread_pool};
    }
  };

  struct Scheduler : ex::scheduler_t {
    WorkSharingThreadPoolBase* thread_pool;
    Sender schedule() const noexcept { return {.thread_pool = thread_pool}; }
    friend bool operator==(const Scheduler& lhs, const Scheduler& rhs) noexcept {
      return lhs.thread_pool == rhs.thread_pool;
    }
  };

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeEasedOperation* operation, uint32_t priority = 0) {
    if constexpr (std::is_same_v<Queue<TypeEasedOperation*>,
                                 internal::util::UnboundedBlockingPriorityQueue<TypeEasedOperation*>>) {
      queue_.Push(operation, priority);
    } else {
      queue_.Push(operation);
    }
  }

 private:
  size_t thread_count_;
  Queue<TypeEasedOperation*> queue_;
  std::vector<std::jthread> workers_;

  void WorkerThreadLoop(const std::stop_token& stop_token) {
    internal::util::SetThreadName("ws_pool_worker");
    while (!stop_token.stop_requested()) {
      auto optional_operation = queue_.Pop(/*timeout_ms=*/10);
      if (!optional_operation) {
        continue;
      }
      optional_operation.value()->Execute();
    }
  }
};

using WorkSharingThreadPool = WorkSharingThreadPoolBase<internal::util::BoundedBlockingQueue>;
using PriorityThreadPool = WorkSharingThreadPoolBase<internal::util::UnboundedBlockingPriorityQueue>;

class WorkStealingThreadPool : public exec::static_thread_pool {
 public:
  using exec::static_thread_pool::static_thread_pool;
  auto GetScheduler() { return get_scheduler(); }
};
}  // namespace ex_actor