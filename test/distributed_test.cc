#include <exception>
#include <memory>
#include <thread>

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

class B {
 public:
  static B Create(int, const std::string&, std::unique_ptr<int>) { return B(); }
};

class C {};

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

  std::string Error() { throw std::runtime_error("error from " + name_); }

  static constexpr std::tuple kActorMethods = {&PingWorker::Ping, &PingWorker::Error};

 private:
  std::string name_;
};

class Error {
 public:
  static Error Create() { return Error(); }

  Error() { throw std::runtime_error("Just an error"); }
};

class Echoer {
 public:
  static Echoer Create() { return Echoer(); }

  std::string Echo(const std::string& message) { return message; }

  std::string Proxy(const std::string& message, const ex_actor::ActorRef<Echoer>& other) {
    auto sender = other.Send<&Echoer::Echo>(message);
    auto [result] = stdexec::sync_wait(std::move(sender)).value();
    return result;
  }

  static constexpr std::tuple kActorMethods = {&Echoer::Echo, &Echoer::Proxy};
};

TEST(DistributedTest, ConstructionInDistributedMode) {
  auto node_main = [](uint32_t this_node_id) {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    ex_actor::ActorRoster<A, B, Error, PingWorker> roster;
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                     /*this_node_id=*/this_node_id, cluster_node_info, roster);

    // test local creation
    auto local_a = registry.CreateActor<A>();
    auto local_b = registry.CreateActor<B>();
    EXPECT_THAT([&registry] { registry.CreateActor<C>(); },
                Throws<std::exception>(Property(&std::exception::what, HasSubstr("Can't find"))));
    auto local_a2 = registry.CreateActorUseStaticCreateFn<A>(ex_actor::ActorConfig {.node_id = this_node_id});

    // test remote creation
    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
    spdlog::info("node {} creating remote actor A", this_node_id);
    auto remote_a =
        registry.CreateActorUseStaticCreateFn<A>(ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "A"});
    spdlog::info("node {} creating remote actor B", this_node_id);
    auto remote_b = registry.CreateActorUseStaticCreateFn<B>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "B"}, 1, "asd", std::make_unique<int>());

    // test remote creation error propagation
    ASSERT_THAT(
        [&]() { registry.CreateActorUseStaticCreateFn<Error>(ex_actor::ActorConfig {.node_id = remote_node_id}); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("Just an error"))));

    ASSERT_THAT(
        [&]() {
          registry.CreateActorUseStaticCreateFn<A>(
              ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "A"});
        },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("same name"))));

    // test remote call
    auto ping_worker =
        registry.CreateActorUseStaticCreateFn<PingWorker>(ex_actor::ActorConfig {.node_id = remote_node_id},
                                                          /*name=*/"Alice");
    auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
    auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
    ASSERT_EQ(ping_res, "ack from Alice, msg got: hello");

    // test remote call error propagation
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
    ex_actor::ActorRoster<Echoer> roster;
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                     /*this_node_id=*/this_node_id, cluster_node_info, roster);

    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
    auto remote_actor = registry.CreateActorUseStaticCreateFn<Echoer>(
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
    ex_actor::ActorRoster<Echoer> roster;
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                     /*this_node_id=*/this_node_id, cluster_node_info, roster);

    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();

    auto local_actor = registry.CreateActor<Echoer>();
    auto remote_actor_a = registry.CreateActorUseStaticCreateFn<Echoer>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "Alice"});
    auto remote_actor_b = registry.CreateActorUseStaticCreateFn<Echoer>(
        ex_actor::ActorConfig {.node_id = remote_node_id, .actor_name = "Bob"});

    std::string msg = "hi";
    // Pass the local actor to remote actor
    auto proxy_sender = remote_actor_a.Send<&Echoer::Proxy>(msg, local_actor);
    auto [proxy_reply] = stdexec::sync_wait(std::move(proxy_sender)).value();
    ASSERT_EQ(proxy_reply, msg);

    // Pass a remote actor to another remote actor at the same remote node
    auto sender = remote_actor_a.Send<&Echoer::Proxy>(msg, remote_actor_b);
    auto [reply] = stdexec::sync_wait(std::move(sender)).value();
    ASSERT_EQ(reply, msg);
  };

  std::jthread node_0(node_main, 0);
  std::jthread node_1(node_main, 1);

  node_0.join();
  node_1.join();
}
