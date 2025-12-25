#include <cassert>
#include <cstdlib>

#include "ex_actor/api.h"

namespace logging = ex_actor::internal::logging;

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  // You can also put this outside the class if you don't want to modify your class
  static PingWorker FactoryCreate(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};

// 1. Register the class & methods using EXA_REMOTE
EXA_REMOTE(&PingWorker::FactoryCreate, &PingWorker::Ping);

namespace {
exec::task<void> MainCoroutine(uint32_t this_node_id, size_t total_nodes) {
  uint32_t remote_node_id = (this_node_id + 1) % total_nodes;

  // 2. Specify the factory function in registry.CreateActor
  auto ping_worker = co_await ex_actor::Spawn<PingWorker, &PingWorker::FactoryCreate>(
      ex_actor::ActorConfig {.node_id = remote_node_id}, /*name=*/"Alice");
  std::string ping_res = co_await ping_worker.Send<&PingWorker::Ping>("hello");
  assert(ping_res == "ack from Alice, msg got: hello");
  (void)ping_res;  // clang-tidy false positive
}
}  // namespace

int main(int /*argc*/, char** argv) {
  auto shared_pool = std::make_shared<ex_actor::WorkSharingThreadPool>(4);
  auto dummy_resource = std::make_shared<int>(0);
  uint32_t this_node_id = std::atoi(argv[1]);
  std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                       {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
  ex_actor::Init(shared_pool->GetScheduler(), this_node_id, cluster_node_info, shared_pool, dummy_resource,
                 dummy_resource, dummy_resource, dummy_resource);
  stdexec::sync_wait(MainCoroutine(this_node_id, cluster_node_info.size()));
  logging::Info("main exit, node id: {}", this_node_id);
}