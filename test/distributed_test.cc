#include <algorithm>
#include <barrier>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_registry.h"
#include "ex_actor/internal/remote_handler_registry.h"

using testing::HasSubstr;
using testing::Property;
using testing::Throws;

namespace logging = ex_actor::internal::log;

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

class D {
 public:
  static D Create() { return D(); }
};
EXA_REMOTE(&D::Create);

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

  exec::task<std::string> Proxy(const std::string& message, const ex_actor::ActorRef<Echoer>& other) {
    auto sender = other.Send<&Echoer::Echo>(message);
    auto result = co_await std::move(sender);
    co_return result;
  }

  exec::task<std::vector<std::string>> ProxyTwoActor(const std::string& message,
                                                     const std::vector<ex_actor::ActorRef<Echoer>>& echoers) {
    std::vector<std::string> strs;
    for (const auto& echoer : echoers) {
      auto sender = echoer.Send<&Echoer::Echo>(message);
      auto reply = co_await std::move(sender);
      strs.push_back(reply);
    }
    co_return strs;
  }
};
EXA_REMOTE(&Echoer::Create, &Echoer::Echo, &Echoer::Proxy, &Echoer::ProxyTwoActor);

struct ProxyEchoer {
  ex_actor::ActorRef<Echoer> echoer;

  exec::task<std::string> Echo(const std::string& str) const {
    auto sender = echoer.Send<&Echoer::Echo>(str);
    auto reply = co_await std::move(sender);
    co_return reply;
  }

  static ProxyEchoer Create(ex_actor::ActorRef<Echoer> echoer) { return ProxyEchoer(echoer); }
};
EXA_REMOTE(&ProxyEchoer::Create, &ProxyEchoer::Echo);

struct RetVoid {
  static RetVoid Create() { return {}; }
  void ReturnVoid() {}
  exec::task<void> CoroutineReturnVoid() { co_return; }
};
EXA_REMOTE(&RetVoid::Create, &RetVoid::ReturnVoid, &RetVoid::CoroutineReturnVoid);

class NonMovableActor {
 public:
  NonMovableActor(const NonMovableActor&) = delete;
  NonMovableActor& operator=(const NonMovableActor&) = delete;
  NonMovableActor(NonMovableActor&&) = delete;
  NonMovableActor& operator=(NonMovableActor&&) = delete;

  static NonMovableActor Create(std::string name) { return NonMovableActor(std::move(name)); }

  std::string GetName() const { return name_; }

 private:
  explicit NonMovableActor(std::string name) : name_(std::move(name)) {}
  std::string name_;
};
EXA_REMOTE(&NonMovableActor::Create, &NonMovableActor::GetName);

TEST(DistributedTest, ConstructionInDistributedModeWithDefaultScheduler) {
  std::barrier bar {2};
  auto node_main = [&bar](size_t index) -> exec::task<void> {
    std::vector<std::string> addresses = {"tcp://127.0.0.1:5301", "tcp://127.0.0.1:5302"};
    ex_actor::ClusterConfig cluster_config {
        .listen_address = addresses.at(index),
        .contact_node_address = (index == 0) ? "" : addresses.at(0),
    };
    ex_actor::ActorRegistry registry(/*thread_pool_size=*/4, cluster_config);

    auto [cluster_state, condition_met] =
        co_await registry.WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                           /*timeout_ms=*/5000);
    EXPECT_TRUE(condition_met);
    EXPECT_GE(cluster_state.nodes.size(), 2U);
    if (cluster_state.nodes.size() < 2U) {
      bar.arrive_and_wait();
      co_return;
    }

    std::string remote_address = addresses.at(1 - index);
    auto it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == remote_address; });
    EXPECT_NE(it, cluster_state.nodes.end());
    if (it == cluster_state.nodes.end()) {
      bar.arrive_and_wait();
      co_return;
    }
    auto remote_node_id = it->node_id;

    auto ping_worker = co_await registry.Spawn<&PingWorker::Create>(/*name=*/"Alice").ToNode(remote_node_id);
    auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
    auto ping_res = co_await std::move(ping);
    EXPECT_EQ(ping_res, "ack from Alice, msg got: hello");
    bar.arrive_and_wait();
  };
  std::jthread node_0([&] { stdexec::sync_wait(node_main(0)); });
  std::jthread node_1([&] { stdexec::sync_wait(node_main(1)); });
}

TEST(DistributedTest, ConstructionInDistributedMode) {
  std::barrier bar {2};
  auto node_main = [&bar](size_t index) -> exec::task<void> {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    std::vector<std::string> addresses = {"tcp://127.0.0.1:5301", "tcp://127.0.0.1:5302"};
    ex_actor::ClusterConfig cluster_config {
        .listen_address = addresses.at(index),
        .contact_node_address = (index == 0) ? "" : addresses.at(0),
    };
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(), cluster_config);

    // test local creation
    auto local_a = co_await registry.Spawn<A>();
    auto local_b = co_await registry.Spawn<B>();
    auto local_a2 = co_await registry.Spawn<A>();

    // test remote creation
    auto [cluster_state, condition_met] =
        co_await registry.WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                           /*timeout_ms=*/5000);
    EXPECT_TRUE(condition_met);
    EXPECT_GE(cluster_state.nodes.size(), 2U);
    if (cluster_state.nodes.size() < 2U) {
      bar.arrive_and_wait();
      co_return;
    }

    std::string this_address = addresses.at(index);
    std::string remote_address = addresses.at(1 - index);
    auto self_it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == this_address; });
    auto remote_it =
        std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == remote_address; });
    EXPECT_NE(self_it, cluster_state.nodes.end());
    EXPECT_NE(remote_it, cluster_state.nodes.end());
    if (self_it == cluster_state.nodes.end() || remote_it == cluster_state.nodes.end()) {
      bar.arrive_and_wait();
      co_return;
    }
    auto this_node_id = self_it->node_id;
    auto remote_node_id = remote_it->node_id;

    // test ToNode with this_node_id (spawn "remotely" on self)
    logging::Info("node {} creating actor on self via ToNode", index);
    auto self_a = co_await registry.Spawn<&A::Create>().ToNode(this_node_id);
    auto self_ping = co_await registry.Spawn<&PingWorker::Create>(/*name=*/"Self").ToNode(this_node_id);
    auto self_reply = co_await self_ping.Send<&PingWorker::Ping>("hi");
    EXPECT_EQ(self_reply, "ack from Self, msg got: hi");

    logging::Info("node {} creating remote actor A", index);
    /*
    before gcc 13, we can't use heap-allocated temp variable after co_await, or there will be a double free error.
    here actor_name is heap allocated. so when using ActorConfig with actor_name, we should define it explicitly.

    i.e. you can't `co_await Spawn<X>().WithConfig({.actor_name = "A"})` with a temporary config containing
    heap-allocated fields, instead, you should define a separate named variable for the config:
    ```cpp
    ex_actor::ActorConfig a_config {.actor_name = "A"};
    auto remote_a = co_await registry.Spawn<&A::Create>().ToNode(remote_node_id).WithConfig(a_config);
    ```

    see https://gcc.gnu.org/pipermail/gcc-bugs/2022-October/800402.html
    */
    ex_actor::ActorConfig a_config {.actor_name = "A"};
    auto remote_a = co_await registry.Spawn<&A::Create>().ToNode(remote_node_id).WithConfig(a_config);

    logging::Info("node {} creating remote actor B", index);
    auto remote_b = co_await registry.Spawn<&B::Create>(1, "asd", std::make_unique<int>()).ToNode(remote_node_id);

    logging::Info("creating remote actor C without registering with EXA_REMOTE");
    auto do_create = [&]() -> void { stdexec::sync_wait(registry.Spawn<&C::Create>().ToNode(remote_node_id)); };
    EXPECT_THAT(do_create, Throws<std::exception>(
                               Property(&std::exception::what, HasSubstr("forgot to register it with EXA_REMOTE"))));

    logging::Info("creating remote actor D without static create function");
    EXPECT_THAT(
        [&]() { stdexec::sync_wait(registry.Spawn<D>().ToNode(remote_node_id)); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("can only be used to create local actor"))));

    // test remote creation error propagation
    auto do_create_error = [&]() -> void {
      stdexec::sync_wait(registry.Spawn<&Error::Create>().ToNode(remote_node_id));
    };
    EXPECT_THAT(do_create_error, Throws<std::exception>(Property(&std::exception::what, HasSubstr("Just an error"))));

    // test remote call
    logging::Info("creating remote actor PingWorker");
    auto ping_worker = co_await registry.Spawn<&PingWorker::Create>(/*name=*/"Alice").ToNode(remote_node_id);
    logging::Info("calling PingWorker::Ping");
    auto sender = ping_worker.Send<&PingWorker::Ping>("hello");
    auto reply = co_await std::move(sender);
    EXPECT_EQ(reply, "ack from Alice, msg got: hello");

    // test call a not registered function
    logging::Info("calling PingWorker::NotRegisteredFunc");
    EXPECT_THAT(
        [&]() -> void { stdexec::sync_wait(ping_worker.Send<&PingWorker::NotRegisteredFunc>()); },
        Throws<std::exception>(Property(&std::exception::what, HasSubstr("forgot to register it with EXA_REMOTE"))));

    // test remote call error propagation
    logging::Info("calling PingWorker::Error");
    auto error = ping_worker.Send<&PingWorker::Error>();
    EXPECT_THAT([&error]() -> void { stdexec::sync_wait(std::move(error)); },
                Throws<std::exception>(Property(&std::exception::what, HasSubstr("error"))));

    // test remote call with void as return value
    logging::Info("calling RetVoid::ReturnVoid and Retvoid::CoroutineReturnVoid");
    auto empty_actor = co_await registry.Spawn<&RetVoid::Create>().ToNode(remote_node_id);
    co_await empty_actor.Send<&RetVoid::ReturnVoid>();
    co_await empty_actor.Send<&RetVoid::CoroutineReturnVoid>();
    bar.arrive_and_wait();
  };

  std::jthread node_0([&] { stdexec::sync_wait(node_main(0)); });
  std::jthread node_1([&] { stdexec::sync_wait(node_main(1)); });

  node_0.join();
  node_1.join();
}

TEST(DistributedTest, ActorLookUpInDistributeMode) {
  std::barrier bar {2};
  auto node_main = [&bar](size_t index) -> exec::task<void> {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    std::vector<std::string> addresses = {"tcp://127.0.0.1:5301", "tcp://127.0.0.1:5302"};
    ex_actor::ClusterConfig cluster_config {
        .listen_address = addresses.at(index),
        .contact_node_address = (index == 0) ? "" : addresses.at(0),
    };
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(), cluster_config);

    auto [cluster_state, condition_met] =
        co_await registry.WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                           /*timeout_ms=*/5000);
    EXPECT_TRUE(condition_met);
    EXPECT_GE(cluster_state.nodes.size(), 2U);
    if (cluster_state.nodes.size() < 2U) {
      bar.arrive_and_wait();
      co_return;
    }

    std::string remote_address = addresses.at(1 - index);
    auto it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == remote_address; });
    EXPECT_NE(it, cluster_state.nodes.end());
    if (it == cluster_state.nodes.end()) {
      bar.arrive_and_wait();
      co_return;
    }
    auto remote_node_id = it->node_id;
    ex_actor::ActorConfig echoer_config {.actor_name = "Alice"};
    auto remote_actor = co_await registry.Spawn<&Echoer::Create>().ToNode(remote_node_id).WithConfig(echoer_config);
    auto lookup_result = co_await registry.GetActorRefByName<Echoer>(remote_node_id, "Alice");
    auto lookup_error = co_await registry.GetActorRefByName<Echoer>(remote_node_id, "A");

    EXPECT_EQ(lookup_result.has_value(), true);
    EXPECT_EQ(lookup_result.value().GetActorId(), remote_actor.GetActorId());
    EXPECT_EQ(lookup_error.has_value(), false);

    auto actor = lookup_result.value();
    std::string msg = "hello";
    auto sender = actor.Send<&Echoer::Echo>(msg);
    auto reply_msg = co_await std::move(sender);
    EXPECT_EQ(reply_msg, msg);
    bar.arrive_and_wait();
  };

  std::jthread node_0([&] { stdexec::sync_wait(node_main(0)); });
  std::jthread node_1([&] { stdexec::sync_wait(node_main(1)); });

  node_0.join();
  node_1.join();
}

TEST(DistributedTest, ActorRefSerializationTest) {
  std::barrier bar {2};
  auto node_main = [&bar](size_t index) -> exec::task<void> {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    std::vector<std::string> addresses = {"tcp://127.0.0.1:5301", "tcp://127.0.0.1:5302"};
    ex_actor::ClusterConfig cluster_config {
        .listen_address = addresses.at(index),
        .contact_node_address = (index == 0) ? "" : addresses.at(0),
    };
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler(), cluster_config);

    auto [cluster_state, condition_met] =
        co_await registry.WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                           /*timeout_ms=*/5000);
    EXPECT_TRUE(condition_met);
    EXPECT_GE(cluster_state.nodes.size(), 2U);
    if (cluster_state.nodes.size() < 2U) {
      bar.arrive_and_wait();
      co_return;
    }

    std::string remote_address = addresses.at(1 - index);
    auto it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == remote_address; });
    EXPECT_NE(it, cluster_state.nodes.end());
    if (it == cluster_state.nodes.end()) {
      bar.arrive_and_wait();
      co_return;
    }
    auto remote_node_id = it->node_id;

    auto local_actor_a = co_await registry.Spawn<Echoer>();
    auto local_actor_b = co_await registry.Spawn<Echoer>();
    ex_actor::ActorConfig echoer_config_a {.actor_name = "Alice"};
    ex_actor::ActorConfig echoer_config_b {.actor_name = "Bob"};
    auto remote_actor_a = co_await registry.Spawn<&Echoer::Create>().ToNode(remote_node_id).WithConfig(echoer_config_a);
    auto remote_actor_b = co_await registry.Spawn<&Echoer::Create>().ToNode(remote_node_id).WithConfig(echoer_config_b);
    std::string msg = "hi";

    // Pass the local actor to remote actor
    auto proxy_sender = remote_actor_a.Send<&Echoer::Proxy>(msg, local_actor_a);
    auto proxy_reply = co_await std::move(proxy_sender);
    EXPECT_EQ(proxy_reply, msg);

    // Pass a remote actor to another remote actor at the same remote node
    auto sender = remote_actor_a.Send<&Echoer::Proxy>(msg, remote_actor_b);
    auto reply = co_await std::move(sender);
    EXPECT_EQ(reply, msg);

    // Pass a vector to the remote actor
    std::vector<ex_actor::ActorRef<Echoer>> echoers = {local_actor_a, local_actor_b};
    auto vec_sender = remote_actor_a.Send<&Echoer::ProxyTwoActor>(msg, echoers);
    auto vec_reply = co_await std::move(vec_sender);
    std::vector<std::string> expected_vec_reply = {msg, msg};
    EXPECT_EQ(vec_reply, expected_vec_reply);

    // Pass a local actor to the constructor of remote actor
    auto proxy_echoer = co_await registry.Spawn<&ProxyEchoer::Create>(local_actor_a).ToNode(remote_node_id);
    auto proxy_echoer_sender = proxy_echoer.Send<&ProxyEchoer::Echo>(msg);
    auto proxy_echoer_reply = co_await std::move(proxy_echoer_sender);
    EXPECT_EQ(proxy_echoer_reply, msg);
    bar.arrive_and_wait();
  };

  std::jthread node_0([&] { stdexec::sync_wait(node_main(0)); });
  std::jthread node_1([&] { stdexec::sync_wait(node_main(1)); });

  node_0.join();
  node_1.join();
}

// Verifies that a non-movable actor can be spawned via a factory function.
// The lambda is never invoked; it only needs to compile, which forces the
// full Spawn -> Actor template chain to be instantiated for NonMovableActor.
TEST(DistributedTest, NonMovableActorCompiles) {
  [[maybe_unused]] auto unused = [](ex_actor::ActorRegistry& registry) {
    return registry.Spawn<&NonMovableActor::Create>(/*name=*/"Bob").ToNode(0);
  };
}
