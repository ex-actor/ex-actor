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

// Usage: <binary> <node_id> <listen_address> <remote_node_id> [contact_address]
exec::task<void> MainCoroutine(int argc, char** argv) {
  auto shared_pool = std::make_shared<ex_actor::WorkSharingThreadPool>(4);
  uint32_t this_node_id = std::atoi(argv[1]);
  std::string listen_address = argv[2];
  uint32_t remote_node_id = std::atoi(argv[3]);
  std::string contact_address = (argc > 4) ? argv[4] : "";
  ex_actor::ClusterConfig cluster_config {
      .this_node_id = this_node_id,
      .listen_address = listen_address,
      .contact_node_address = contact_address,
  };
  ex_actor::Init(shared_pool->GetScheduler(), cluster_config);
  ex_actor::HoldResource(shared_pool);

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
