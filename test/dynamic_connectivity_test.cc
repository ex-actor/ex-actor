#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <thread>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_registry.h"
#include "ex_actor/internal/logging.h"
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

class StarTest {
 public:
  StarTest(uint32_t this_node_id, uint32_t cluster_size) : this_node_id_(this_node_id), cluster_size_(cluster_size) {};
  void test() {
    ex_actor::ClusterConfig config;
    auto base_port = 5000;
    config.this_node = {.node_id = this_node_id_,
                        .address = fmt::format("tcp://127.0.0.1:{}", base_port + this_node_id_)};
    config.network_config = {.heartbeat_timeout = std::chrono::milliseconds {1000},
                             .gossip_interval = std::chrono::milliseconds {100},
                             .gossip_fanout = cluster_size_ / 2};

    std::random_device device;
    auto random_generator = std::mt19937(device());
    std::uniform_int_distribution<uint32_t> dist(0, cluster_size_ - 1);
    uint32_t target_node_id = dist(random_generator);
    if (this_node_id_ != 0) {
      config.contact_node = {.node_id = 0, .address = fmt::format("tcp://127.0.0.1:{}", base_port)};
    }
    ex_actor::Init(1, config);
    auto [connected] =
        stdexec::sync_wait(ex_actor::WaitNodeAlive(target_node_id, std::chrono::milliseconds {3000})).value();
    // Sync all  nodes

    EXA_THROW_CHECK(connected) << fmt::format("Error: this node {} can't connected to node {}/n", this_node_id_,
                                              target_node_id);

    auto actor_creation_sender = ex_actor::Spawn<Echoer>();
    auto [actor_ref] = stdexec::sync_wait(actor_creation_sender).value();
    std::string str {"Hello"};
    auto actor_method_calling_sender = actor_ref.Send<&Echoer::Echo>(str);
    auto [method_calling_return_val] = stdexec::sync_wait(std::move(actor_method_calling_sender)).value();
    EXA_THROW_CHECK(method_calling_return_val == str) << fmt::format("Error: local method calling error");

    auto remote_actor_creation_sender = ex_actor::Spawn<Echoer, &Echoer::Create>({.node_id = target_node_id});
    auto [remote_actor_ref] = stdexec::sync_wait(std::move(remote_actor_creation_sender)).value();
    auto remote_actor_calling_sender = actor_ref.Send<&Echoer::Echo>(str);
    auto [remote_method_calling_return_val] = stdexec::sync_wait(std::move(remote_actor_calling_sender)).value();
    EXA_THROW_CHECK(remote_method_calling_return_val == str) << fmt::format("Error: local method calling error");

    // Sync all  nodes
    std::this_thread::sleep_for(std::chrono::milliseconds {1500});
    ex_actor::Shutdown();
  }

 private:
  uint32_t this_node_id_;
  uint32_t cluster_size_;
};

int main(int argc, char* argv[]) {
  if (argc < 2) {
    return 1;
  }

  uint32_t cluster_size = std::atoi(argv[1]);
  uint32_t this_node_id = std::atoi(argv[2]);
  StarTest star_test(this_node_id, cluster_size);
  star_test.test();

  return 0;
}
