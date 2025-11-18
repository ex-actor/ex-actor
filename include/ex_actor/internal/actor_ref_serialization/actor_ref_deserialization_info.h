#pragma  once
#include "ex_actor/internal/actor.h"
#include "ex_actor/internal/network.h"
namespace ex_actor::internal {
struct ActorRefDeserializationInfo {
  uint32_t this_node_id = 0;
  std::function<TypeErasedActor*(uint64_t)> actor_look_up_fn;
  network::MessageBroker* message_broker = nullptr;
};
}  // namespace ex_actor::internal
