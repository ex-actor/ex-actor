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

int main(int /*argc*/, char** argv) {
  ex_actor::internal::InstallFallbackExceptionHandler();
  uint32_t this_node_id = std::atoi(argv[1]);
  auto coroutine = [](uint32_t this_node_id) -> exec::task<void> {
    std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                         {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
    ex_actor::ClusterConfig cluster_config {.this_node = cluster_node_info.at(this_node_id)};
    if (this_node_id != cluster_node_info.front().node_id) {
      cluster_config.contact_node = cluster_node_info.front();
    }
    ex_actor::Init(/*thread_pool_size=*/4, cluster_config);
    uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
    bool connected = co_await ex_actor::WaitNodeAlive(remote_node_id, 5000);
    EXA_THROW_CHECK(connected) << "Cannot connect to node " << remote_node_id;
    auto ping_worker = co_await ex_actor::Spawn<&PingWorker::Create>(/*name=*/"Alice").ToNode(remote_node_id);
    while (true) {
      try {
        auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
        std::ignore = co_await std::move(ping);
      } catch (const std::exception& e) {
        logging::Error("Node {} caught exception during ping: {}", this_node_id, e.what());
        break;
      }
    }
    co_await ex_actor::WaitOsExitSignal();
    ex_actor::Shutdown();
  };
  stdexec::sync_wait(coroutine(this_node_id));
}
