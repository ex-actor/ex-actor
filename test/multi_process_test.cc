#include <cassert>
#include <cstdlib>

#include "ex_actor/api.h"
#include "ex_actor/internal/logging.h"

namespace {
namespace logging = ex_actor::internal::log;
class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  static PingWorker CreateFn(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};
EXA_REMOTE(&PingWorker::CreateFn, &PingWorker::Ping);

exec::task<void> MainCoroutine(int argc, char** argv) {
  auto shared_pool = std::make_shared<ex_actor::WorkSharingThreadPool>(4);
  uint32_t this_node_id = std::atoi(argv[1]);
  std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                       {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
  ex_actor::ClusterConfig cluster_config {.this_node = cluster_node_info.at(this_node_id)};
  if (this_node_id != cluster_node_info.front().node_id) {
    cluster_config.contact_node = cluster_node_info.front();
  }
  ex_actor::Init(shared_pool->GetScheduler(), cluster_config);
  ex_actor::HoldResource(shared_pool);

  uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
  bool connected = co_await ex_actor::WaitNodeAlive(remote_node_id, 5000);
  EXA_THROW_CHECK(connected) << "Cannot connected to node " << remote_node_id;

  // 2. Specify the create function in Spawn
  auto ping_worker = co_await ex_actor::Spawn<&PingWorker::CreateFn>(/*name=*/"Alice").ToNode(remote_node_id);
  std::string ping_res = co_await ping_worker.Send<&PingWorker::Ping>("hello");
  assert(ping_res == "ack from Alice, msg got: hello");
  (void)ping_res;

  logging::Info("All work done, node id: {}", this_node_id);
  co_await ex_actor::WaitOsExitSignal();
  ex_actor::Shutdown();
}
}  // namespace

int main(int argc, char** argv) { stdexec::sync_wait(MainCoroutine(argc, argv)); }
