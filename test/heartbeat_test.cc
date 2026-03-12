#include <chrono>
#include <exception>
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

// Usage: <binary> <node_id> <listen_address> <remote_node_id> [contact_address]
int main(int argc, char** argv) {
  ex_actor::internal::InstallFallbackExceptionHandler();
  uint32_t this_node_id = std::atoi(argv[1]);
  std::string listen_address = argv[2];
  uint32_t remote_node_id = std::atoi(argv[3]);
  std::string contact_address = (argc > 4) ? argv[4] : "";
  auto coroutine = [](uint32_t this_node_id, std::string listen_address, uint32_t remote_node_id,
                      std::string contact_address) -> exec::task<void> {
    ex_actor::ClusterConfig cluster_config {
        .this_node_id = this_node_id,
        .listen_address = listen_address,
        .contact_node_address = contact_address,
        .network_config =
            {
                .heartbeat_timeout_ms = 500,
                .gossip_interval_ms = 50,
            },
    };
    ex_actor::Init(/*thread_pool_size=*/2, cluster_config);
    bool connected = co_await ex_actor::WaitNodeAlive(remote_node_id, 5000);
    EXA_THROW_CHECK(connected) << "Cannot connect to node " << remote_node_id;
    auto ping_worker = co_await ex_actor::Spawn<&PingWorker::Create>(/*name=*/"Alice").ToNode(remote_node_id);
    logging::Info("Node {} connected to node {}, remote actor created, now start to ping it", this_node_id,
                  remote_node_id);
    while (true) {
      try {
        auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
        std::ignore = co_await std::move(ping);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } catch (const std::exception& e) {
        logging::Error("Node {} caught exception during ping: {}", this_node_id, e.what());
        break;
      }
    }
    co_await ex_actor::WaitOsExitSignal();
    ex_actor::Shutdown();
  };
  stdexec::sync_wait(coroutine(this_node_id, std::move(listen_address), remote_node_id, std::move(contact_address)));
}
