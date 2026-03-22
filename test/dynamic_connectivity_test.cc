#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "ex_actor/api.h"

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

enum class TestType : uint8_t {
  kStar,
  kChain,
};

class DynamicConnectivityTest {
 public:
  DynamicConnectivityTest(uint32_t this_index, uint32_t cluster_size, TestType type)
      : this_index_(this_index), cluster_size_(cluster_size), type_(type) {}

  void Test() {
    auto config = BuildConfig();
    auto coroutine = [this, config]() -> exec::task<void> {
      co_await ex_actor::Start(/*thread_pool_size=*/1);
      co_await ex_actor::StartOrJoinCluster(config);

      // Wait for all other nodes to be discovered
      auto [cluster_state, condition_met] =
          co_await ex_actor::WaitClusterState([this](const auto& state) { return state.nodes.size() >= cluster_size_; },
                                              /*timeout_ms=*/6000);
      EXA_THROW_CHECK(condition_met) << ex_actor::fmt_lib::format(
          "Error: node index {} can't discover all {} nodes, only found {}", this_index_, cluster_size_,
          cluster_state.nodes.size());

      // Pick a target node by address
      uint32_t target_index = PickTargetIndex();
      std::string target_address = GetAddress(target_index);
      auto it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == target_address; });
      EXA_THROW_CHECK(it != cluster_state.nodes.end()) << "Cannot find target node at " << target_address;
      auto target_node_id = it->node_id;

      std::string str {"Hello"};
      auto actor_ref = co_await ex_actor::Spawn<Echoer>();
      auto method_calling_return_val = co_await actor_ref.Send<&Echoer::Echo>(str);
      EXA_THROW_CHECK(method_calling_return_val == str)
          << ex_actor::fmt_lib::format("Error: local method calling error");

      auto remote_actor_ref = co_await ex_actor::Spawn<&Echoer::Create>().ToNode(target_node_id);
      auto remote_method_calling_return_val = co_await remote_actor_ref.Send<&Echoer::Echo>(str);
      EXA_THROW_CHECK(remote_method_calling_return_val == str)
          << ex_actor::fmt_lib::format("Error: remote method calling error");
      std::cout << "All work done" << std::endl;
      co_await ex_actor::WaitOsExitSignal();
      co_await ex_actor::Stop();
    };

    stdexec::sync_wait(coroutine());
  }

 private:
  static std::string GetAddress(uint32_t index) {
    constexpr uint32_t kBasePort = 5000;
    return ex_actor::fmt_lib::format("tcp://127.0.0.1:{}", kBasePort + index);
  }

  ex_actor::ClusterConfig BuildConfig() const {
    ex_actor::ClusterConfig config;
    config.listen_address = GetAddress(this_index_);
    config.network_config = {
        .heartbeat_timeout_ms = 1000,
        .gossip_interval_ms = 100,
        .gossip_fanout = cluster_size_ > 1 ? cluster_size_ / 2 : 1,
    };

    if (this_index_ != 0) {
      uint32_t contact_index = 0;
      if (type_ == TestType::kChain) {
        contact_index = this_index_ - 1;
      }
      config.contact_node_address = GetAddress(contact_index);
    }
    return config;
  }

  uint32_t PickTargetIndex() const {
    uint32_t target_index;
    if (type_ == TestType::kStar) {
      std::random_device device;
      std::mt19937 random_generator(device());
      std::uniform_int_distribution<uint32_t> dist(0, cluster_size_ - 1);
      target_index = dist(random_generator);
    } else {
      target_index = cluster_size_ - 1 - this_index_;
      if (target_index == this_index_) {
        target_index = (this_index_ + 1) % cluster_size_;
      }
    }
    return target_index;
  }

  uint32_t this_index_;
  uint32_t cluster_size_;
  TestType type_;
};

int main(int argc, char* argv[]) {
  if (argc < 4) {
    return -1;
  }

  uint32_t cluster_size = std::atoi(argv[1]);
  uint32_t this_index = std::atoi(argv[2]);
  if (cluster_size == 0 || this_index >= cluster_size) {
    return 1;
  }

  TestType type;
  std::string type_str = argv[3];
  if (type_str == "star") {
    type = TestType::kStar;
  } else if (type_str == "chain") {
    type = TestType::kChain;
  } else {
    return -1;
  }

  DynamicConnectivityTest dynamic_connectivity_test(this_index, cluster_size, type);
  dynamic_connectivity_test.Test();

  return 0;
}
