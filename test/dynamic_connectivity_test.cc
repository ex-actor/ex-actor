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

int main(int argc, char* argv[]) {
  if (argc < 2) {
    return 1;
  }

  uint32_t cluster_size = std::atoi(argv[1]);
  uint32_t this_node_id = std::atoi(argv[2]);
  ex_actor::ClusterConfig config;
  auto base_port = 5000;
  config.this_node = {.node_id = this_node_id, .address = fmt::format("tcp://127.0.0.1:{}", base_port + this_node_id)};
  config.network_config = {.heartbeat_timeout = std::chrono::milliseconds {1000},
                           .gossip_interval = std::chrono::milliseconds {100},
                           .gossip_fanout = cluster_size / 2};

  std::random_device device;
  auto random_generator = std::mt19937(device());
  std::uniform_int_distribution<uint32_t> dist(0, cluster_size - 1);
  uint32_t target_node_id = dist(random_generator);
  if (this_node_id != 0) {
    config.contact_node = {.node_id = 0, .address = fmt::format("tcp://127.0.0.1:{}", base_port)};
  }
  ex_actor::Init(1, config);
  auto [connected] =
      stdexec::sync_wait(ex_actor::WaitNodeAlive(target_node_id, std::chrono::milliseconds {3000})).value();
  std::this_thread::sleep_for(std::chrono::milliseconds {1000});

  EXA_THROW_CHECK(connected) << fmt::format("Error: this node {} can't connected to node {}/n", this_node_id,
                                            target_node_id);

  auto actor_creation_sender = ex_actor::Spawn<Echoer>();
  auto [actor_ref] = stdexec::sync_wait(actor_creation_sender).value();

  ex_actor::Shutdown();

  return 0;
}
