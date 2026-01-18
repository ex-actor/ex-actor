#include <cassert>
#include <cstdint>

#include "ex_actor/api.h"
#include "ex_actor/internal/actor_registry.h"

int main(int /*argc*/, char* argv[]) {
  uint32_t node_id = std::atoi(argv[1]);
  ex_actor::ClusterConfig config;

  if (node_id == 0) {
    config = ex_actor::ClusterConfig {.this_node = {.node_id = 0, .address = "tcp://127.0.0.1:5555"}};
  } else {
    config = ex_actor::ClusterConfig {.this_node = {.node_id = 1, .address = "tcp://127.0.0.1:6666"},
                                      .contact_node = {.node_id = 0, .address = "tcp://127.0.0.1:5555"}};
  }

  ex_actor::Init(1, config);
  bool result {false};
  if (node_id == 0) {
    result = ex_actor::WaitNode(1, std::chrono::milliseconds {2000});
  } else {
    result = true;
    std::this_thread::sleep_for(std::chrono::milliseconds {2000});
  }

  assert(result);
  ex_actor::Shutdown();
  return 0;
}
