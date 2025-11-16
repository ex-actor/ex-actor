#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "spdlog/common.h"

using testing::HasSubstr;
using testing::Property;
using testing::Throws;

class A {
 public:
  static A Create() { return A(); }
};
EXA_REMOTE(&A::Create);

class B {
 public:
  static B Create(int, const std::string&, std::unique_ptr<int>) { return B(); }
};
EXA_REMOTE(&B::Create);

class C {
 public:
  static C Create() { return C(); }
};

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

  std::string Error() { throw std::runtime_error("error from " + name_); }

  void NotRegisteredFunc() {}

 private:
  std::string name_;
};
EXA_REMOTE(&PingWorker::Create, &PingWorker::Ping, &PingWorker::Error);

class Error {
 public:
  static Error Create() { return Error(); }

  Error() { throw std::runtime_error("Just an error"); }
};
EXA_REMOTE(&Error::Create);

class Echoer {
 public:
  static Echoer Create() { return Echoer(); }

  std::string Echo(const std::string& message) { return message; }

  std::string Proxy(const std::string& message, const ex_actor::ActorRef<Echoer>& other) {
    auto sender = other.Send<&Echoer::Echo>(message);
    auto [result] = stdexec::sync_wait(std::move(sender)).value();
    return result;
  }

  std::vector<std::string> ProxyTwoActor(const std::string& message,
                                         const std::vector<ex_actor::ActorRef<Echoer>>& echoers) {
    std::vector<std::string> strs;
    for (const auto& echoer : echoers) {
      auto sender = echoer.Send<&Echoer::Echo>(message);
      auto [reply] = stdexec::sync_wait(std::move(sender)).value();
      strs.push_back(reply);
    }
    return strs;
  }
};
EXA_REMOTE(&Echoer::Create, &Echoer::Echo, &Echoer::Proxy, &Echoer::ProxyTwoActor);

struct ProxyEchoer {
  ex_actor::ActorRef<Echoer> echoer;

  std::string Echo(const std::string& str) const {
    auto sender = echoer.Send<&Echoer::Echo>(str);
    auto [reply] = stdexec::sync_wait(std::move(sender)).value();
    return reply;
  }

  static ProxyEchoer Create(ex_actor::ActorRef<Echoer> echoer) { return ProxyEchoer(echoer); }
};
EXA_REMOTE(&ProxyEchoer::Create, &ProxyEchoer::Echo);

TEST(DistributedTest, ConstructionInDistributedModeWithDefaultScheduler) {
  auto node_main = [](uint32_t this_node_id) {
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};

    ex_actor::ActorRegistry registry(/*thread_pool_size=*/4,
                                     /*this_node_id=*/this_node_id, cluster_node_info);

    // test remote creation
    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
    auto ping_worker =
        registry.CreateActor<PingWorker, &PingWorker::Create>(ex_actor::ActorConfig {.node_id = remote_node_id},
                                                              /*name=*/"Alice");
    auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
    auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
    ASSERT_EQ(ping_res, "ack from Alice, msg got: hello");
  };
  std::jthread node_0(node_main, 0);
  std::jthread node_1(node_main, 1);
}

TEST(DistributedTest, ConstructionInDistributedMode) {
  auto node_main = [](uint32_t this_node_id) {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                     /*this_node_id=*/this_node_id, cluster_node_info);

    // test local creation
    auto local_a = registry.CreateActor<A>();
    auto local_b = registry.CreateActor<B>();
    auto local_a2 = registry.CreateActor<A>(ex_actor::ActorConfig {.node_id = this_node_id});

    // test remote creation
    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();

    spdlog::info("node {} creating remote actor A", this_node_id);
    auto remote_a =
        registry.CreateActor<A, &A::Create>(ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "A"});

    spdlog::info("node {} creating remote actor B", this_node_id);
    auto remote_b = registry.CreateActor<B, &B::Create>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "B"}, 1, "asd", std::make_unique<int>());

    spdlog::info("creating remote actor C without static create function");
    ASSERT_THAT(
        [&]() { registry.CreateActor<C>(ex_actor::ActorConfig {.node_id = remote_node_id}); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("can only be used to create local actor"))));

    spdlog::info("creating remote actor C without registering with EXA_REMOTE");
    auto do_create = [&]() { registry.CreateActor<C, &C::Create>(ex_actor::ActorConfig {.node_id = remote_node_id}); };
    ASSERT_THAT(do_create, Throws<std::exception>(
                               Property(&std::exception::what, HasSubstr("forgot to register it with EXA_REMOTE"))));

    // test remote creation error propagation
    auto do_create_error = [&]() {
      registry.CreateActor<Error, &Error::Create>(ex_actor::ActorConfig {.node_id = remote_node_id});
    };
    ASSERT_THAT(do_create_error, Throws<std::exception>(Property(&std::exception::what, HasSubstr("Just an error"))));

    // test remote call
    spdlog::info("creating remote actor PingWorker");
    auto ping_worker =
        registry.CreateActor<PingWorker, &PingWorker::Create>(ex_actor::ActorConfig {.node_id = remote_node_id},
                                                              /*name=*/"Alice");

    // test call a not registered function
    spdlog::info("calling PingWorker::NotRegisteredFunc");
    ASSERT_THAT(
        [&]() { stdexec::sync_wait(ping_worker.Send<&PingWorker::NotRegisteredFunc>()); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("forgot to register it with EXA_REMOTE"))));

    spdlog::info("calling PingWorker::Ping");
    auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
    auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
    ASSERT_EQ(ping_res, "ack from Alice, msg got: hello");

    // test remote call error propagation
    spdlog::info("calling PingWorker::Error");
    auto error = ping_worker.Send<&PingWorker::Error>();
    ASSERT_THAT([&error] { stdexec::sync_wait(std::move(error)); },
                Throws<std::exception>(Property(&std::exception::what, HasSubstr("error"))));
  };

  std::jthread node_0(node_main, 0);
  std::jthread node_1(node_main, 1);

  node_0.join();
  node_1.join();
}

TEST(DistributedTest, ActorLookUpInDistributeMode) {
  auto node_main = [](uint32_t this_node_id) {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                     /*this_node_id=*/this_node_id, cluster_node_info);

    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
    auto remote_actor = registry.CreateActor<Echoer, &Echoer::Create>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "Alice"});
    auto lookup_result = registry.GetActorRefByName<Echoer>(remote_node_id, "Alice");
    auto lookup_error = registry.GetActorRefByName<Echoer>(remote_node_id, "A");

    ASSERT_EQ(lookup_result.has_value(), true);
    ASSERT_EQ(lookup_result.value().GetActorId(), remote_actor.GetActorId());
    ASSERT_EQ(lookup_error.has_value(), false);

    auto actor = lookup_result.value();
    std::string msg = "hello";
    auto sender = actor.Send<&Echoer::Echo>(msg);
    auto reply = stdexec::sync_wait(std::move(sender));
    ASSERT_EQ(reply.has_value(), true);
    auto [reply_msg] = reply.value();
    ASSERT_EQ(reply_msg, msg);
  };

  std::jthread node_0(node_main, 0);
  std::jthread node_1(node_main, 1);

  node_0.join();
  node_1.join();
}

TEST(DistributedTest, ActorRefSerializationTest) {
  auto node_main = [](uint32_t this_node_id) {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                     /*this_node_id=*/this_node_id, cluster_node_info);

    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();

    auto local_actor_a = registry.CreateActor<Echoer>();
    auto local_actor_b = registry.CreateActor<Echoer>();
    auto remote_actor_a = registry.CreateActor<Echoer, &Echoer::Create>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "Alice"});
    auto remote_actor_b = registry.CreateActor<Echoer, &Echoer::Create>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "Bob"});
    std::string msg = "hi";

    // Pass the local actor to remote actor
    auto proxy_sender = remote_actor_a.Send<&Echoer::Proxy>(msg, local_actor_a);
    auto [proxy_reply] = stdexec::sync_wait(std::move(proxy_sender)).value();
    ASSERT_EQ(proxy_reply, msg);

    // Pass a remote actor to another remote actor at the same remote node
    auto sender = remote_actor_a.Send<&Echoer::Proxy>(msg, remote_actor_b);
    auto [reply] = stdexec::sync_wait(std::move(sender)).value();
    ASSERT_EQ(reply, msg);

    // Pass a vector to the remote actor
    std::vector<ex_actor::ActorRef<Echoer>> echoers = {local_actor_a, local_actor_b};
    auto vec_sender = remote_actor_a.Send<&Echoer::ProxyTwoActor>(msg, echoers);
    auto [vec_reply] = stdexec::sync_wait(std::move(vec_sender)).value();
    std::vector<std::string> expected_vec_reply = {msg, msg};
    ASSERT_EQ(vec_reply, expected_vec_reply);

    // Pass a local actor to the constructor of remote actor
    auto proxy_echoer = registry.CreateActor<ProxyEchoer, &ProxyEchoer::Create>(
        ex_actor::ActorConfig {.node_id = remote_node_id}, local_actor_a);
    auto proxy_echoer_sender = proxy_echoer.Send<&ProxyEchoer::Echo>(msg);
    auto [proxy_echoer_reply] = stdexec::sync_wait(std::move(proxy_echoer_sender)).value();
    ASSERT_EQ(proxy_echoer_reply, msg);
  };

  std::jthread node_0(node_main, 0);
  std::jthread node_1(node_main, 1);

  node_0.join();
  node_1.join();
}
