#include <cassert>

#include "ex_actor/api.h"

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  // Boilerplate 1, the Create method
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

  // Boilerplate 2, the kActorMethods tuple.
  // The name can be adjusted by defining `EXA_ACTOR_METHODS_KEYWORD`
  static constexpr std::tuple kActorMethods = {&PingWorker::Ping};

 private:
  std::string name_;
};

int main(int /*argc*/, char** argv) {
  uint32_t this_node_id = std::atoi(argv[1]);
  ex_actor::WorkSharingThreadPool thread_pool(4);

  // Boilerplate 3, the actor roster(a name list of all actors)
  // split by comma, e.g. ActorRoster<A, B, C...>
  ex_actor::ActorRoster<PingWorker> roster;

  std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                       {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                   /*this_node_id=*/this_node_id, cluster_node_info, roster);

  uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
  auto ping_worker =
      registry.CreateActorUseStaticCreateFn<PingWorker>(ex_actor::ActorConfig {.node_id = remote_node_id},
                                                        /*name=*/"Alice");
  auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
  auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
  assert(ping_res == "ack from Alice, msg got: hello");
}