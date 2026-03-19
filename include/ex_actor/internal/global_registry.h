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

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "ex_actor/internal/actor_ref.h"
#include "ex_actor/internal/actor_registry.h"
#include "ex_actor/internal/logging.h"
#include "ex_actor/internal/network.h"
#include "ex_actor/internal/reflect.h"

namespace ex_actor::internal {
ActorRegistry& GetGlobalDefaultRegistry();
void AssignGlobalDefaultRegistry(std::unique_ptr<ActorRegistry> registry);
bool IsGlobalDefaultRegistryInitialized();
void SetupGlobalHandlers();
}  // namespace ex_actor::internal

namespace ex_actor {
using ex_actor::internal::ActorRegistry;
using ex_actor::internal::SenderOf;

/// This is exposed to make public API signature clean.
/// It's not encouraged to use it, we don't promise to keep it stable.
using ex_actor::internal::FnReturnType;

/**
 * @brief Init the global default registry in single-node mode, use the default thread pool as the scheduler.
 * @note Not thread-safe.
 */
void Init(uint32_t thread_pool_size);

/**
 * @brief Init the global default registry in single-node mode, use user-specified scheduler.
 * @note Not thread-safe.
 */
template <ex::scheduler Scheduler>
void Init(Scheduler scheduler);

/**
 * @brief [Experimental] Switch to distributed mode by starting or joining a cluster.
 * @note Not thread-safe. Should be called after Init() and before Shutdown(). Can't be called twice.
 */
exec::task<void> StartOrJoinCluster(const ClusterConfig& cluster_config);

/**
 * @brief Shutdown the global default registry. Must be called explicitly before `main` exits.
 * @note Not thread-safe
 */
void Shutdown();

/**
 * @brief Register signal handlers for SIGINT and SIGTERM, and return a task that completes when one of them is
 * received. Useful for long-running server processes that should stay alive until interrupted.
 * @note Your original signal handler won't be lost, we'll call yours after our code. When this function
 * finishes, we'll restore your original signal handler.
 * @warning Don't set signal handlers during the middle of this function, we don't promise safety in such case.
 */
exec::task<void> WaitOsExitSignal();

/**
 * @brief Wait until a predicate on the set of alive nodes is satisfied, or timeout.
 * @return The list of alive nodes and whether the condition was met before the timeout.
 */
exec::task<WaitClusterStateResult> WaitClusterState(std::function<bool(const ClusterState&)> predicate,
                                                    uint64_t timeout_ms);

/**
 * @brief Ask ex_actor to hold a resource, the resource won't be released until runtime is shut down.
 * Often used to hold an execution resource when using custom scheduler.
 * @param resource The resource to hold. Can pass any `unique_ptr` or `shared_ptr`.
 * @note Not thread-safe.
 */
void HoldResource(std::shared_ptr<void> resource);

/**
 * @brief Spawn a local actor. Returns a SpawnBuilder (which is a sender) that can optionally be configured
 * with .ToNode() and/or .WithConfig() before awaiting.
 *
 * @code
 * // Default (local node, default config):
 * auto ref = co_await ex_actor::Spawn<Counter>();
 *
 * // With config:
 * auto ref = co_await ex_actor::Spawn<Counter>().WithConfig({.actor_name = "counter1"});
 * @endcode
 */
template <class UserClass, class... Args>
SenderOf<ActorRef<UserClass>> auto Spawn(Args... args) {
  return internal::GetGlobalDefaultRegistry().Spawn<UserClass, Args...>(std::move(args)...);
}

/**
 * @brief Spawn an actor (local or remote) using a factory function. Returns a SpawnBuilder (which is a sender)
 * that can optionally be configured with .ToNode() and/or .WithConfig() before awaiting.
 *
 * @code
 * // Default (local node):
 * auto ref = co_await ex_actor::Spawn<&Worker::Create>("worker1");
 *
 * // Spawn on a remote node:
 * auto ref = co_await ex_actor::Spawn<&Worker::Create>("worker1").ToNode(2);
 * @endcode
 */
template <auto kCreateFn, class... Args>
SenderOf<ActorRef<FnReturnType<kCreateFn>>> auto Spawn(Args... args) {
  return internal::GetGlobalDefaultRegistry().Spawn<kCreateFn, Args...>(std::move(args)...);
}

/**
 * @brief Destroy an actor.
 */
template <class UserClass>
SenderOf<> auto DestroyActor(const ActorRef<UserClass>& actor_ref) {
  return internal::GetGlobalDefaultRegistry().DestroyActor<UserClass>(actor_ref);
}

/**
 * @brief Find the actor by name at current node.
 */
template <class UserClass>
SenderOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(name);
}

/**
 * @brief Find the actor by name at specified node.
 */
template <class UserClass>
SenderOf<std::optional<ActorRef<UserClass>>> auto GetActorRefByName(const uint64_t& node_id, const std::string& name) {
  return internal::GetGlobalDefaultRegistry().GetActorRefByName<UserClass>(node_id, name);
}

/**
 * @brief Configure the logging of ex_actor.
 * @note Not thread-safe, please call it only when no logs are printing.
 */
void ConfigureLogging(const LogConfig& config = {});

}  // namespace ex_actor

// -----------template function implementations(non-template funcs are written in .cc file)-------------

namespace ex_actor {

template <ex::scheduler Scheduler>
void Init(Scheduler scheduler) {
  internal::log::Info("Initializing ex_actor with custom scheduler.");
  EXA_THROW_CHECK(!internal::IsGlobalDefaultRegistryInitialized()) << "Already initialized.";
  AssignGlobalDefaultRegistry(std::make_unique<ActorRegistry>(std::move(scheduler)));
  internal::SetupGlobalHandlers();
}

}  // namespace ex_actor
