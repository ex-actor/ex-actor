#include <algorithm>
#include <cassert>
#include <string>

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

// Usage: <binary> <listen_address> <node_name> <peer_node_name> [contact_address]
stdexec::task<void> MainCoroutine(int argc, char** argv) {
  EXA_THROW_CHECK(argc >= 4) << "Usage: <binary> <listen_address> <node_name> <peer_node_name> [contact_address]";
  auto shared_pool = std::make_shared<ex_actor::WorkSharingThreadPool>(4);
  std::string listen_address = argv[1];
  std::string node_name = argv[2];
  std::string peer_node_name = argv[3];
  std::string contact_address = (argc > 4) ? argv[4] : "";
  ex_actor::ClusterConfig cluster_config {
      .listen_address = listen_address,
      .contact_node_address = contact_address,
      .network_config = {.heartbeat_timeout_ms = 5000},
      .node_name = node_name,
  };
  ex_actor::Init(shared_pool->GetScheduler());
  co_await ex_actor::StartOrJoinCluster(cluster_config);
  ex_actor::HoldResource(shared_pool);

  auto [cluster_state, condition_met] = co_await ex_actor::WaitClusterState(
      [&](const auto& state) {
        return std::ranges::any_of(state.nodes, [&](const auto& n) { return n.node_name == peer_node_name; });
      },
      /*timeout_ms=*/5000);
  EXA_THROW_CHECK(condition_met) << "Cannot find peer node with name " << peer_node_name;

  auto it = std::ranges::find_if(cluster_state.nodes,
                                 [&](const auto& n) { return n.node_name == peer_node_name; });
  EXA_THROW_CHECK(it != cluster_state.nodes.end()) << "Cannot find peer node with name " << peer_node_name;
  auto remote_node_id = it->node_id;

  auto ping_worker = co_await ex_actor::Spawn<&PingWorker::CreateFn>(/*name=*/"Alice").ToNode(remote_node_id);
  std::string ping_res = co_await ping_worker.Send<&PingWorker::Ping>("hello");
  assert(ping_res == "ack from Alice, msg got: hello");
  (void)ping_res;

  logging::Info("All work done");
  co_await ex_actor::WaitOsExitSignal();
  ex_actor::Shutdown();
}
}  // namespace

int main(int argc, char** argv) { stdexec::sync_wait(MainCoroutine(argc, argv)); }
