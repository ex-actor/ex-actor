// Copyright 2026 The ex_actor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ex_actor/internal/global_registry.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "ex_actor/internal/constants.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"

// ----------------------Global Default Registry--------------------------

namespace {
std::vector<std::shared_ptr<void>> resource_holder;
std::unique_ptr<ex_actor::ActorRegistry> global_default_registry;
}  // namespace

namespace ex_actor::internal {
ex_actor::ActorRegistry& GetGlobalDefaultRegistry() {
  EXA_THROW_CHECK(IsGlobalDefaultRegistryInitialized()) << "Global default registry is not initialized.";
  return *global_default_registry;
}

void AssignGlobalDefaultRegistry(std::unique_ptr<ex_actor::ActorRegistry> registry) {
  global_default_registry = std::move(registry);
}

bool IsGlobalDefaultRegistryInitialized() { return global_default_registry != nullptr; }

static void RegisterAtExitCleanup() {
  static bool at_exit_cleanup_registered = false;

  if (at_exit_cleanup_registered) {
    return;
  }
  at_exit_cleanup_registered = true;

  // Because Init() is called after main starts, this handler will be called before static object destruction.
  // Once we enter static object destruction without calling Shutdown(), the program will crash due to static object
  // destruction order fiasco. So we print an error message here and force exit the program.
  //
  // We can't call Shutdown() here for user, because thread_local objects are already destroyed, shutting down
  // ActorRegistry needs to use the mailbox, which has some thread_local objects, the program will crash if we call
  // Shutdown() here. Printing an error message is the best we can do here. User need to call Shutdown() explicitly
  // before main() exits.
  std::atexit([] {
    if (!IsGlobalDefaultRegistryInitialized()) {
      return;
    }
    log::Error(
        "ex_actor is not shutdown when exiting main(), calling std::quick_exit(1) to force exit, resources may not be "
        "cleaned properly. To fix this error, call ex_actor::Shutdown() before main() exits.");
    std::quick_exit(1);
  });
}

void SetupGlobalHandlers() { RegisterAtExitCleanup(); }

ClusterConfig BuildClusterConfigFromNodeList(uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  ClusterConfig cluster_config {
      .this_node = NodeInfo {.node_id = this_node_id},
  };
  NodeInfo min_id_node {.node_id = this_node_id};

  for (const auto& node : cluster_node_info) {
    if (node.node_id < min_id_node.node_id) {
      min_id_node.node_id = node.node_id;
      min_id_node.address = node.address;
    }

    if (node.node_id == this_node_id) {
      cluster_config.this_node.address = node.address;
    }
  }
  if (min_id_node.node_id != this_node_id) {
    cluster_config.contact_node = min_id_node;
  }
  return cluster_config;
}

void WaitForClusterNodes(uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  for (const auto& node : cluster_node_info) {
    if (node.node_id != this_node_id) {
      auto [connected] =
          stdexec::sync_wait(ex_actor::WaitNodeAlive(node.node_id, kDefaultWaitNodeAliveTimeoutMs)).value();
      EXA_THROW_CHECK(connected) << "Cannot connect to the node " << node.node_id;
    }
  }
}
}  // namespace ex_actor::internal

namespace ex_actor {
void Init(uint32_t thread_pool_size) {
  internal::log::Info("Initializing ex_actor in single-node mode with default scheduler, thread_pool_size={}",
                      thread_pool_size);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size);
  internal::SetupGlobalHandlers();
}

void Init(uint32_t thread_pool_size, uint32_t this_node_id, const std::vector<NodeInfo>& cluster_node_info) {
  internal::log::Info(
      "Initializing ex_actor in distributed mode with default scheduler, thread_pool_size={}, this_node_id={}, "
      "total_nodes={}",
      thread_pool_size, this_node_id, cluster_node_info.size());
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  auto cluster_config = internal::BuildClusterConfigFromNodeList(this_node_id, cluster_node_info);
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size, cluster_config);
  internal::SetupGlobalHandlers();
  internal::WaitForClusterNodes(this_node_id, cluster_node_info);
}

void Init(uint32_t thread_pool_size, const ClusterConfig& cluster_config) {
  internal::log::Info(
      "Initializing ex_actor in distributed mode with default scheduler, thread_pool_size={}, this_node_id={}, ",
      thread_pool_size, cluster_config.this_node.node_id);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size, cluster_config);
  internal::SetupGlobalHandlers();
}

void HoldResource(std::shared_ptr<void> resource) { resource_holder.push_back(std::move(resource)); }

exec::task<bool> WaitNodeAlive(uint32_t node_id, uint64_t timeout_ms) {
  co_return co_await global_default_registry->WaitNodeAlive(node_id, timeout_ms);
}

void Shutdown() {
  internal::log::Info("Shutting down ex_actor.");
  EXA_THROW_CHECK(internal::IsGlobalDefaultRegistryInitialized()) << "Not initialized.";
  global_default_registry.reset();
  resource_holder.clear();
}

void ConfigureLogging(const LogConfig& config) { internal::GlobalLogger() = internal::CreateLoggerUsingConfig(config); }

}  // namespace ex_actor
