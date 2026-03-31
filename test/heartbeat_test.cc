#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "ex_actor/api.h"
#include "ex_actor/internal/logging.h"

namespace logging = ex_actor::internal::log;

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};

EXA_REMOTE(&PingWorker::Create, &PingWorker::Ping);

// Usage: <binary> <listen_address> <remote_address> [contact_address]
int main(int argc, char** argv) {
  ex_actor::internal::InstallFallbackExceptionHandler();
  std::string listen_address = argv[1];
  std::string remote_address = argv[2];
  std::string contact_address = (argc > 3) ? argv[3] : "";
  auto coroutine = [](std::string listen_address, std::string remote_address,
                      std::string contact_address) -> exec::task<void> {
    ex_actor::ClusterConfig cluster_config {
        .listen_address = listen_address,
        .contact_node_address = contact_address,
        .network_config =
            {
                .heartbeat_timeout_ms = 500,
                .gossip_interval_ms = 50,
            },
    };
    ex_actor::Init(/*thread_pool_size=*/2);
    co_await ex_actor::StartOrJoinCluster(cluster_config);

    auto [cluster_state, condition_met] =
        co_await ex_actor::WaitClusterState([](const auto& state) { return state.nodes.size() >= 2; },
                                            /*timeout_ms=*/5000);
    EXA_THROW_CHECK(condition_met) << "Cannot connect to any remote node";

    auto it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address == remote_address; });
    EXA_THROW_CHECK(it != cluster_state.nodes.end()) << "Cannot find remote node at " << remote_address;
    auto remote_node_id = it->node_id;

    auto ping_worker = co_await ex_actor::Spawn<&PingWorker::Create>(/*name=*/"Alice").ToNode(remote_node_id);
    logging::Info("Connected to remote node, remote actor created, now start to ping it");
    while (true) {
      try {
        auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
        std::ignore = co_await std::move(ping);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } catch (const ex_actor::NetworkError& e) {
        logging::Error("connection lost to node {:#x}: {}", e.remote_node_id, e.what());
        break;
      }
    }
    co_await ex_actor::WaitOsExitSignal();
    ex_actor::Shutdown();
  };
  stdexec::sync_wait(coroutine(std::move(listen_address), std::move(remote_address), std::move(contact_address)));
}
