#include <memory>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/util.h"

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

  static constexpr std::tuple kActorMethods = {&PingWorker::Ping};

 private:
  std::string name_;
};

TEST(DistributedTest, ConstructionInDistributedMode) {
  ex_actor::util::Semaphore done_nodes(2);
  auto node_main = [&done_nodes](uint32_t this_node_id) {
    ex_actor::WorkSharingThreadPool thread_pool(4);
    ex_actor::ActorRoster<A, B, PingWorker> roster;
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
    auto remote_a = registry.CreateActorUseStaticCreateFn<A>(ex_actor::ActorConfig {.node_id = remote_node_id});
    spdlog::info("node {} creating remote actor B", this_node_id);
    auto remote_b = registry.CreateActorUseStaticCreateFn<B>(ex_actor::ActorConfig {.node_id = remote_node_id}, 1,
                                                             "asd", std::make_unique<int>());

    // test remote call
    auto ping_worker =
        registry.CreateActorUseStaticCreateFn<PingWorker>(ex_actor::ActorConfig {.node_id = remote_node_id},
                                                          /*name=*/"Alice");
    auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
    auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
    ASSERT_EQ(ping_res, "ack from Alice, msg got: hello");

    // wait other nodes done before destroying actor registry, or message will be lost
    done_nodes.Acquire(1);
    ex_actor::ex::sync_wait(done_nodes.OnDrained());
    spdlog::info("node {} done", this_node_id);
  };

  std::jthread node_0(node_main, 0);
  std::jthread node_1(node_main, 1);

  node_0.join();
  node_1.join();
}

// SCENARIO("construction exceptions should be propagated") {}

// SCENARIO("remote call exceptions should be propagated") {}

// SCENARIO("actor ref can pass through network") {}

// SCENARIO("get remote actor by its name") {}