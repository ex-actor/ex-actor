#include <thread>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = ex_actor::ex;

// ==========================================================
// Test 1: Support arbitrary user-defined tags from ActorConfig
//   (third-party scheduler pattern)
// ==========================================================

// 1a. Define a custom tag
struct my_custom_tag_t {
  auto operator()(const auto& env) const noexcept {
    auto v = env.query(my_custom_tag_t {});
    // AnyStdExecScheduler::ActorEnv returns std::variant
    return std::get<int64_t>(v);
  }
};
constexpr inline my_custom_tag_t my_custom_tag {};

// 1b. Define a simple custom scheduler that captures the env value
struct MyCustomScheduler {
  int64_t* captured_value;

  template <typename R>
  struct Operation {
    R receiver;
    int64_t* captured_value;

    Operation(R receiver, int64_t* captured_value) : receiver(std::move(receiver)), captured_value(captured_value) {}

    void start() noexcept {
      auto env = ex::get_env(receiver);
      try {
        *captured_value = my_custom_tag(env);
        ex::set_value(std::move(receiver));
      } catch (...) {
        ex::set_error(std::move(receiver), std::current_exception());
      }
    }
  };

  struct Sender : ex::sender_t {
    int64_t* captured_value;
    using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_error_t(std::exception_ptr)>;

    explicit Sender(int64_t* captured_value) : captured_value(captured_value) {}

    template <ex::receiver R>
    Operation<R> connect(R receiver) const {
      return Operation<R> {std::move(receiver), captured_value};
    }
  };

  Sender schedule() const noexcept { return Sender {captured_value}; }

  friend bool operator==(const MyCustomScheduler& lhs, const MyCustomScheduler& rhs) noexcept {
    return lhs.captured_value == rhs.captured_value;
  }
};

static_assert(ex::scheduler<MyCustomScheduler>);

struct TestActorForCustomTag {};

TEST(UserDefinedSchedulerTest, ShouldSupportArbitraryEnvsInActorConfig) {
  int64_t captured = -1;
  MyCustomScheduler my_scheduler {&captured};

  ex_actor::ActorRegistry registry(my_scheduler);

  ex_actor::ActorConfig config;
  config.SetSchedulerEnv<my_custom_tag_t>(int64_t(998877));

  auto result = ex::sync_wait(registry.Spawn<TestActorForCustomTag>().WithConfig(config));
  ASSERT_TRUE(result.has_value());

  EXPECT_EQ(captured, 998877);
}

// ==========================================================
// Test 2: User-defined scheduler with core binding
// ==========================================================

// 2a. User-defined config tag: CPU core index
struct get_core_index_t {
  auto operator()(const auto& env) const noexcept {
    auto v = env.query(get_core_index_t {});
    if (std::holds_alternative<uint64_t>(v)) {
      return std::get<uint64_t>(v);
    }
    return uint64_t(0);
  }
};
constexpr inline get_core_index_t get_core_index {};

// 2b. User-defined scheduler that simulates core-bound thread pool
class CoreBoundThreadPool {
 public:
  struct OperationBase {
    virtual ~OperationBase() = default;
    virtual void Execute() = 0;
  };

  template <typename R>
  struct Operation : OperationBase {
    R receiver;
    CoreBoundThreadPool* pool;

    Operation(R receiver, CoreBoundThreadPool* pool) : receiver(std::move(receiver)), pool(pool) {}

    void start() noexcept {
      auto env = ex::get_env(receiver);
      uint64_t target_core = get_core_index(env);

      pool->last_bound_core = target_core;

      ex::set_value(std::move(receiver));
    }

    void Execute() override { ex::set_value(std::move(receiver)); }
  };

  struct Scheduler {
    CoreBoundThreadPool* pool;

    struct Sender : ex::sender_t {
      CoreBoundThreadPool* pool;
      using completion_signatures = ex::completion_signatures<ex::set_value_t()>;

      explicit Sender(CoreBoundThreadPool* pool) : pool(pool) {}

      template <ex::receiver R>
      Operation<R> connect(R receiver) const {
        return {std::move(receiver), pool};
      }
    };

    Sender schedule() const noexcept { return Sender {pool}; }

    bool operator==(const Scheduler& rhs) const { return pool == rhs.pool; }
  };

  Scheduler GetScheduler() { return {this}; }

  // Records the last bound core for verification
  std::atomic<uint64_t> last_bound_core {999};
};

struct BindableActor {
  void DoWork() {}
};

TEST(UserDefinedSchedulerTest, ShouldSupportUserDefinedTagsAndSchedulers) {
  CoreBoundThreadPool pool;
  ex_actor::ActorRegistry registry(pool.GetScheduler());

  ex_actor::ActorConfig config;
  config.SetSchedulerEnv<get_core_index_t>(uint64_t(7));

  auto result = ex::sync_wait(registry.Spawn<BindableActor>().WithConfig(config));
  ASSERT_TRUE(result.has_value());
  auto actor = std::move(std::get<0>(*result));

  ex::sync_wait(actor.Send<&BindableActor::DoWork>());

  EXPECT_EQ(pool.last_bound_core.load(), 7);
}
