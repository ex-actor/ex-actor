#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

#include "ex_actor/api.h"

int main(int argc, char* argv[]) {
  if (argc < 2) {
    return 1;
  }
  uint32_t node_id = std::atoi(argv[1]);
  ex_actor::ClusterConfig config;
  config.network_config = {.gossip_interval = std::chrono::milliseconds {100}};

  if (node_id == 0) {
    config.this_node = {.node_id = 0, .address = "tcp://127.0.0.1:5555"};
  } else {
    config.this_node = {.node_id = 1, .address = "tcp://127.0.0.1:6666"};
    config.contact_node = {.node_id = 0, .address = "tcp://127.0.0.1:5555"};
  }
  ex_actor::Init(1, config);
  bool result = false;
  if (node_id == 0) {
    auto [alive] = ex_actor::ex::sync_wait(ex_actor::WaitNodeAlive(1, std::chrono::milliseconds {3000})).value();
    std::cout << fmt::format("node 1 is {}, This node id is 0\n", alive);
    auto [missing] = ex_actor::ex::sync_wait(ex_actor::WaitNodeAlive(2, std::chrono::milliseconds {500})).value();
    std::cout << fmt::format("node 2 is {}, This node id is 0\n", missing);
    result = alive && !missing;
  } else {
    auto [alive] = ex_actor::ex::sync_wait(ex_actor::WaitNodeAlive(0, std::chrono::milliseconds {3000})).value();
    std::this_thread::sleep_for(std::chrono::milliseconds {500});
    std::cout << fmt::format("node 0 is {}, This node id is 1\n", alive);
    result = alive;
  }
  ex_actor::Shutdown();
  return result ? 0 : 1;
}
