#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <stdexec/execution.hpp>
#include <exec/task.hpp>
#include <exec/async_scope.hpp>
#include <string>
#include <stdexcept>
#include <vector>
#include "ex_actor/api.h"
#include "ex_actor/internal/actor.h"

using namespace ex_actor;
namespace ex = stdexec;

namespace debug_info_test_ns {

class ActorC {
public:
    void ThrowError() {
        throw std::runtime_error("Exception in ActorC");
    }
};

class ActorD {
public:
    int Success() {
        return 42;
    }
};

class ActorE {
public:
    void ThrowNonStd() {
        throw "String Exception (Non-std)";
    }
    
    void ThrowStd() {
        throw std::runtime_error("Nested Failure in E");
    }
};

class ActorB {
public:
    exec::task<void> CallC(ActorRef<ActorC> c_ref) {
        // Build the chain: B -> C
        co_await c_ref.Send<&ActorC::ThrowError>().AttachDebugInfo("B calling C");
        co_return;
    }

    exec::task<int> CallCandD(ActorRef<ActorC> c_ref, ActorRef<ActorD> d_ref) {
        exec::async_scope scope;
        auto f1 = scope.spawn_future(c_ref.Send<&ActorC::ThrowError>().AttachDebugInfo("B calling C (concurrent)"));
        auto f2 = scope.spawn_future(d_ref.Send<&ActorD::Success>().AttachDebugInfo("B calling D (concurrent)"));
        
        co_await scope.on_empty();
        
        // This will throw because C failed
        co_await std::move(f1);
        co_return co_await std::move(f2);
    }

    exec::task<void> CallDandE(ActorRef<ActorD> d_ref, ActorRef<ActorE> e_ref) {
        exec::async_scope scope;
        auto f1 = scope.spawn_future(d_ref.Send<&ActorD::Success>().AttachDebugInfo("B calling D (nested-concurrent)"));
        auto f2 = scope.spawn_future(e_ref.Send<&ActorE::ThrowStd>().AttachDebugInfo("B calling E (nested-concurrent)"));
        
        co_await scope.on_empty();
        co_await std::move(f1);
        co_await std::move(f2);
    }

    exec::task<std::string> CallCWithTryCatch(ActorRef<ActorC> c_ref) {
        try {
            co_await c_ref.Send<&ActorC::ThrowError>().AttachDebugInfo("B calling C (guarded)");
        } catch (const internal::log::ActorException& e) {
            // Internally caught and returned as a string for verification
            co_return std::string(e.what());
        }
        co_return "Not Caught";
    }

    exec::task<void> CallCMinimal(ActorRef<ActorC> c_ref) {
        // No AttachDebugInfo here!
        co_await c_ref.Send<&ActorC::ThrowError>();
        co_return;
    }
};

class ActorA {
public:
    exec::task<void> CallB(ActorRef<ActorB> b_ref, ActorRef<ActorC> c_ref) {
        // Build the chain: A -> B -> C
        co_await b_ref.Send<&ActorB::CallC>(c_ref).AttachDebugInfo("A calling B");
        co_return;
    }

    exec::task<int> CallBConcurrent(ActorRef<ActorB> b_ref, ActorRef<ActorC> c_ref, ActorRef<ActorD> d_ref) {
        co_return co_await b_ref.Send<&ActorB::CallCandD>(c_ref, d_ref).AttachDebugInfo("A calling B (concurrent)");
    }

    exec::task<void> CallBAndCConcurrent(ActorRef<ActorB> b_ref, ActorRef<ActorC> c_ref, ActorRef<ActorD> d_ref, ActorRef<ActorE> e_ref) {
        exec::async_scope scope;
        auto f1 = scope.spawn_future(b_ref.Send<&ActorB::CallDandE>(d_ref, e_ref).AttachDebugInfo("A calling B (nested)"));
        auto f2 = scope.spawn_future(c_ref.Send<&ActorC::ThrowError>().AttachDebugInfo("A calling C (nested)"));

        co_await scope.on_empty();
        co_await std::move(f1);
        co_await std::move(f2);
    }
};

} // namespace debug_info_test_ns

using namespace debug_info_test_ns;
struct ExActorGuard {
    explicit ExActorGuard(uint32_t threads = 4) { ex_actor::Init(threads); }
    ~ExActorGuard() { ex_actor::Shutdown(); }
};

TEST(DebugInfoTest, DistributedStackTrace) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<void> {
        auto a = co_await ex_actor::Spawn<ActorA>();
        auto b = co_await ex_actor::Spawn<ActorB>();
        auto c = co_await ex_actor::Spawn<ActorC>();

        // Start the chain
        co_await a.Send<&ActorA::CallB>(b, c).AttachDebugInfo("Start User Task #123");
        co_return;
    };

    // In the NEW scheme, we expect ActorException
    try {
        stdexec::sync_wait(coroutine());
        FAIL() << "Should have thrown an exception";
    } catch (const internal::log::ActorException& e) {
        // Root node logs the full distributed stack trace
        internal::log::Error("Caught Distributed Stack Trace:\n{}", e.what());

        const auto& data = e.GetData();
        // The frames should contain our messages in reverse order (since built backward)
        EXPECT_FALSE(data.stack_trace.empty());
        
        bool found_c = false;
        bool found_b = false;
        bool found_a = false;
        for (const auto& frame : data.stack_trace) {
            auto s = frame.ToString();
            if (s.find("ActorC") != std::string::npos) found_c = true;
            if (s.find("B calling C") != std::string::npos) found_b = true;
            if (s.find("A calling B") != std::string::npos) found_a = true;
        }
        EXPECT_TRUE(found_c);
        EXPECT_TRUE(found_b);
        EXPECT_TRUE(found_a);
    } catch (const std::exception& e) {
        FAIL() << "Should have caught ActorException, but caught: " << e.what();
    }
}

TEST(DebugInfoTest, ConcurrentCallsTest) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<int> {
        auto a = co_await ex_actor::Spawn<ActorA>();
        auto b = co_await ex_actor::Spawn<ActorB>();
        auto c = co_await ex_actor::Spawn<ActorC>();
        auto d = co_await ex_actor::Spawn<ActorD>();

        // In this case, B calls C and D concurrently. C fails.
        co_return co_await a.Send<&ActorA::CallBConcurrent>(b, c, d).AttachDebugInfo("Start Concurrent Task");
    };

    try {
        stdexec::sync_wait(coroutine());
        FAIL() << "Should have thrown an exception because C failed";
    } catch (const internal::log::ActorException& e) {
        internal::log::Error("Caught Concurrent Task Exception:\n{}", e.what());
        
        // Ensure the trace points to the concurrent branch that failed (C)
        bool found_c_concurrent = false;
        for (const auto& frame : e.GetData().stack_trace) {
            if (frame.ToString().find("B calling C (concurrent)") != std::string::npos) {
                found_c_concurrent = true;
                break;
            }
        }
        EXPECT_TRUE(found_c_concurrent);
    }
}

TEST(DebugInfoTest, TryCatchInActorTest) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<std::string> {
        auto b = co_await ex_actor::Spawn<ActorB>();
        auto c = co_await ex_actor::Spawn<ActorC>();

        // B will catch the exception from C and return it as a string
        co_return co_await b.Send<&ActorB::CallCWithTryCatch>(c).AttachDebugInfo("Start Try-Catch Task");
    };

    auto [result] = stdexec::sync_wait(coroutine()).value();
    internal::log::Info("Caught trace in actor B via try-catch:\n{}", result);
    
    EXPECT_TRUE(result.find("ActorC") != std::string::npos);
    EXPECT_TRUE(result.find("B calling C (guarded)") != std::string::npos);
}

TEST(DebugInfoTest, MinimalTracingTest) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<void> {
        auto b = co_await ex_actor::Spawn<ActorB>();
        auto c = co_await ex_actor::Spawn<ActorC>();

        // No AttachDebugInfo in the whole chain
        co_await b.Send<&ActorB::CallCMinimal>(c);
        co_return;
    };

    try {
        stdexec::sync_wait(coroutine());
        FAIL() << "Should have thrown an exception";
    } catch (const internal::log::ActorException& e) {
        internal::log::Error("Caught Minimal Trace:\n{}", e.what());
        
        // We still expect to see at least the actor names because they are added by Actor's let_error
        const auto& data = e.GetData();
        bool found_c = false;
        bool found_b = false;
        for (const auto& frame : data.stack_trace) {
            auto s = frame.ToString();
            if (s.find("ActorC") != std::string::npos) found_c = true;
            if (s.find("ActorB") != std::string::npos) found_b = true;
        }
        EXPECT_TRUE(found_b);
    }
}

TEST(DebugInfoTest, NestedConcurrencyTest) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<void> {
        auto a = co_await ex_actor::Spawn<ActorA>();
        auto b = co_await ex_actor::Spawn<ActorB>();
        auto c = co_await ex_actor::Spawn<ActorC>();
        auto d = co_await ex_actor::Spawn<ActorD>();
        auto e = co_await ex_actor::Spawn<ActorE>();

        co_await a.Send<&ActorA::CallBAndCConcurrent>(b, c, d, e).AttachDebugInfo("Extreme Nesting");
    };

    try {
        stdexec::sync_wait(coroutine());
        FAIL() << "Should have thrown";
    } catch (const internal::log::ActorException& ex) {
        internal::log::Info("Caught Nested Concurrency Exception:\n{}", ex.what());
        // Verify we see the specific path to failure among multiple branches
        EXPECT_TRUE(std::string(ex.what()).find("ActorE::ThrowStd") != std::string::npos);
        EXPECT_TRUE(std::string(ex.what()).find("A calling B (nested)") != std::string::npos);
    }
}

TEST(DebugInfoTest, CustomNameTest) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<void> {
        ex_actor::ActorConfig config {.actor_name = "MasterChef"};
        auto c = co_await ex_actor::Spawn<ActorC>().WithConfig(config);

        co_await c.Send<&ActorC::ThrowError>().AttachDebugInfo("Cooking Disaster");
    };

    try {
        stdexec::sync_wait(coroutine());
    } catch (const internal::log::ActorException& e) {
        internal::log::Info("Caught Custom Name Exception:\n{}", e.what());
        // We simplified the trace, so we expect the class name, but not necessarily the instance name
        EXPECT_TRUE(std::string(e.what()).find("ActorC") != std::string::npos);
    }
}

TEST(DebugInfoTest, NonStdExceptionTest) {
    ExActorGuard guard;

    auto coroutine = []() -> exec::task<void> {
        auto e = co_await ex_actor::Spawn<ActorE>();
        co_await e.Send<&ActorE::ThrowNonStd>().AttachDebugInfo("Risky Move");
    };

    try {
        stdexec::sync_wait(coroutine());
        FAIL() << "Should have thrown";
    } catch (const internal::log::ActorException& ex) {
        internal::log::Info("Caught Non-Std Exception Trace:\n{}", ex.what());
        EXPECT_TRUE(std::string(ex.what()).find("Unknown Exception") != std::string::npos);
        EXPECT_TRUE(std::string(ex.what()).find("ActorE::ThrowNonStd") != std::string::npos);
    }
}
