#pragma once

#if __has_include(<blockingconcurrentqueue.h>)
#include <blockingconcurrentqueue.h>
#elif __has_include(<moodycamel/blockingconcurrentqueue.h>)
#include <moodycamel/blockingconcurrentqueue.h>
#else
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#endif

#include <thread>

#include <exec/static_thread_pool.hpp>

#include "ex_actor/detail/util.h"

namespace ex_actor {

class WorkSharingThreadPool {
 public:
  explicit WorkSharingThreadPool(size_t thread_count) {
    workers_.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this](const std::stop_token& stop_token) { WorkerThreadLoop(stop_token); });
    }
  }

  struct TypeEasedOperation {
    virtual ~TypeEasedOperation() = default;
    virtual void Execute() = 0;
  };

  template <ex::receiver R>
  struct Operation : TypeEasedOperation {
    Operation(R receiver, WorkSharingThreadPool* thread_pool)
        : receiver(std::move(receiver)), thread_pool(thread_pool) {}
    R receiver;
    WorkSharingThreadPool* thread_pool;
    void Execute() override { receiver.set_value(); }

    void start() noexcept { thread_pool->EnqueueOperation(this); }
  };

  struct Scheduler;

  struct Sender : ex::sender_t {
    // NOLINTNEXTLINE(readability-identifier-naming)
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    WorkSharingThreadPool* thread_pool;
    struct Env {
      WorkSharingThreadPool* thread_pool;
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
    WorkSharingThreadPool* thread_pool;
    Sender schedule() const noexcept { return {.thread_pool = thread_pool}; }
    friend bool operator==(const Scheduler& lhs, const Scheduler& rhs) noexcept {
      return lhs.thread_pool == rhs.thread_pool;
    }
  };

  Scheduler GetScheduler() noexcept { return Scheduler {.thread_pool = this}; }

  void EnqueueOperation(TypeEasedOperation* operation) { queue_.enqueue(operation); }

 private:
  moodycamel::BlockingConcurrentQueue<TypeEasedOperation*> queue_;
  std::vector<std::jthread> workers_;

  void WorkerThreadLoop(const std::stop_token& stop_token) {
    TypeEasedOperation* operation = nullptr;
    while (!stop_token.stop_requested()) {
      bool ok = queue_.wait_dequeue_timed(operation, std::chrono::milliseconds(100));
      if (!ok) {
        continue;
      }
      operation->Execute();
    }
  }
};

class WorkStealingThreadPool : public exec::static_thread_pool {
 public:
  using exec::static_thread_pool::static_thread_pool;
  auto GetScheduler() { return get_scheduler(); }
};
}  // namespace ex_actor