#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ex_actor/api.h"
#include "spdlog/fmt/bundled/format.h"

using std::chrono::milliseconds;

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
  DynamicConnectivityTest(uint32_t this_node_id, uint32_t cluster_size, TestType type)
      : this_node_id_(this_node_id), cluster_size_(cluster_size), type_(type) {}

  void Test() {
    auto config = BuildConfig();
    ex_actor::Init(1, config);
    CheckConnectivityAndCallMethod();
    std::this_thread::sleep_for(std::chrono::milliseconds {1500});
    ex_actor::Shutdown();
  }

 private:
  ex_actor::ClusterConfig BuildConfig() const {
    ex_actor::ClusterConfig config;
    constexpr uint32_t kBasePort = 5000;
    config.this_node = {.node_id = this_node_id_,
                        .address = fmt::format("tcp://127.0.0.1:{}", kBasePort + this_node_id_)};
    config.network_config = {
        .heartbeat_timeout = std::chrono::milliseconds {1000},
        .gossip_interval = std::chrono::milliseconds {100},
        .gossip_fanout = cluster_size_ > 1 ? cluster_size_ / 2 : 1,
    };

    if (this_node_id_ != 0) {
      uint32_t contact_node_id = 0;
      if (type_ == TestType::kChain) {
        contact_node_id = this_node_id_ - 1;
      }
      config.contact_node = {.node_id = contact_node_id,
                             .address = fmt::format("tcp://127.0.0.1:{}", kBasePort + contact_node_id)};
    }
    return config;
  }

  void CheckConnectivityAndCallMethod() const {
    uint32_t target_node_id;
    if (type_ == TestType::kStar) {
      std::random_device device;
      std::mt19937 random_generator(device());
      std::uniform_int_distribution<uint32_t> dist(0, cluster_size_ - 1);
      target_node_id = dist(random_generator);
    } else {
      target_node_id = cluster_size_ - 1 - this_node_id_;
      if (target_node_id == this_node_id_) {
        target_node_id = (this_node_id_ + 1) % cluster_size_;
      }
    }

    auto [connected] =
        stdexec::sync_wait(ex_actor::WaitNodeAlive(target_node_id, std::chrono::milliseconds {6000})).value();
    EXA_THROW_CHECK(connected) << fmt::format("Error: node {} can't connected to node {}", this_node_id_,
                                              target_node_id);

    std::string str {"Hello"};
    auto actor_creation_sender = ex_actor::Spawn<Echoer>();
    auto [actor_ref] = stdexec::sync_wait(actor_creation_sender).value();
    auto actor_method_calling_sender = actor_ref.Send<&Echoer::Echo>(str);
    auto [method_calling_return_val] = stdexec::sync_wait(std::move(actor_method_calling_sender)).value();
    EXA_THROW_CHECK(method_calling_return_val == str) << fmt::format("Error: local method calling error");

    auto remote_actor_creation_sender = ex_actor::Spawn<Echoer, &Echoer::Create>({.node_id = target_node_id});
    auto [remote_actor_ref] = stdexec::sync_wait(std::move(remote_actor_creation_sender)).value();
    auto remote_actor_calling_sender = remote_actor_ref.Send<&Echoer::Echo>(str);
    auto [remote_method_calling_return_val] = stdexec::sync_wait(std::move(remote_actor_calling_sender)).value();
    EXA_THROW_CHECK(remote_method_calling_return_val == str) << fmt::format("Error: remote method calling error");
  }

  uint32_t this_node_id_;
  uint32_t cluster_size_;
  TestType type_;
};

int main(int argc, char* argv[]) {
  if (argc < 4) {
    return -1;
  }

  uint32_t cluster_size = std::atoi(argv[1]);
  uint32_t this_node_id = std::atoi(argv[2]);
  if (cluster_size == 0 || this_node_id >= cluster_size) {
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

  DynamicConnectivityTest dynamic_connectivity_test(this_node_id, cluster_size, type);
  dynamic_connectivity_test.Test();

  return 0;
}
