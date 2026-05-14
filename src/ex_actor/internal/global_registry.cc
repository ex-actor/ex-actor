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

#include <csignal>
#include <cstdint>
#include <memory>
#include <vector>

#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/util.h"

// ----------------------Global Default Registry--------------------------

namespace {
std::vector<std::shared_ptr<void>> resource_holder;
std::unique_ptr<ex_actor::ActorRegistry> global_default_registry;

ex_actor::Semaphore* signal_semaphore = nullptr;

void (*prev_sigint_handler)(int) = SIG_DFL;
void (*prev_sigterm_handler)(int) = SIG_DFL;

void InvokePreviousHandler(void (*handler)(int), int sig) {
  if (handler != SIG_DFL && handler != SIG_IGN && handler != nullptr) {
    handler(sig);
  }
}

void ShutdownSignalHandler(int sig) {
  if (signal_semaphore != nullptr) {
    signal_semaphore->Acquire(1);
  }

  switch (sig) {
    case SIGINT:
      InvokePreviousHandler(prev_sigint_handler, sig);
      break;
    case SIGTERM:
      InvokePreviousHandler(prev_sigterm_handler, sig);
      break;
    default:
      break;
  }
}
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

}  // namespace ex_actor::internal

namespace ex_actor {
void Init(uint32_t thread_pool_size) {
  internal::log::Info("Initializing ex_actor with default scheduler, thread_pool_size={}", thread_pool_size);
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  global_default_registry = std::make_unique<ActorRegistry>(thread_pool_size);
  internal::SetupGlobalHandlers();
}

ex::task<void> StartOrJoinCluster(const ClusterConfig& cluster_config) {
  EXA_THROW_CHECK(internal::IsGlobalDefaultRegistryInitialized()) << "Not initialized. Call Init() first.";
  return internal::GetGlobalDefaultRegistry().StartOrJoinCluster(cluster_config);
}

void HoldResource(std::shared_ptr<void> resource) { resource_holder.push_back(std::move(resource)); }

ex::task<void> WaitOsExitSignal() {
  Semaphore sem(1);
  signal_semaphore = &sem;

  prev_sigint_handler = std::signal(SIGINT, ShutdownSignalHandler);
  prev_sigterm_handler = std::signal(SIGTERM, ShutdownSignalHandler);

  co_await sem.OnDrained();

  std::signal(SIGINT, prev_sigint_handler);
  std::signal(SIGTERM, prev_sigterm_handler);
  signal_semaphore = nullptr;
}

void Shutdown() {
  EXA_THROW_CHECK(internal::IsGlobalDefaultRegistryInitialized()) << "Not initialized.";
  internal::log::Info("Shutting down ex_actor.");
  global_default_registry.reset();
  resource_holder.clear();
}

uint64_t GetNodeId() { return internal::GetGlobalDefaultRegistry().GetNodeId(); }

void ConfigureLogging(const LogConfig& config) { internal::SetGlobalLogger(internal::CreateLoggerUsingConfig(config)); }

}  // namespace ex_actor
