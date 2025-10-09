#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>

#include "ex_actor/detail/actor.h"
#include "ex_actor/detail/reflect.h"

namespace ex_actor {

template <class UserClass>
class ActorRef {
 public:
  ActorRef() : is_valid_(false) {}

  ActorRef(uint32_t node_index, uint64_t actor_id, detail::TypeErasedActor* actor)
      : is_valid_(true), node_id_(node_index), actor_id_(actor_id), type_erased_actor_(actor) {}

  friend bool operator==(const ActorRef& lhs, const ActorRef& rhs) {
    if (lhs.is_valid_ == false && rhs.is_valid_ == false) {
      return true;
    }
    return lhs.node_id_ == rhs.node_id_ && lhs.actor_id_ == rhs.actor_id_;
  }

  struct SendRequest {
    const ActorRef* actor_ref;
    size_t mailbox_partition_index;

    template <auto kMethod, class... Args>
    ex::sender auto Send(Args&&... args) const {
      if (!actor_ref->IsValid()) [[unlikely]] {
        throw std::runtime_error("Empty ActorRef, cannot call method on it.");
      }
      return actor_ref->type_erased_actor_->template CallActorMethod<UserClass, kMethod>(mailbox_partition_index,
                                                                                         std::forward<Args>(args)...);
    }
  };

  SendRequest SubMailbox(size_t mailbox_partition_index) const {
    return SendRequest {.actor_ref = this, .mailbox_partition_index = mailbox_partition_index};
  }

  template <auto kMethod, class... Args>
  ex::sender auto Send(Args&&... args) const {
    return SubMailbox(0).template Send<kMethod>(std::forward<Args>(args)...);
  }

  bool IsValid() const { return is_valid_; }

  uint32_t GetNodeId() const { return node_id_; }
  uint64_t GetActorId() const { return actor_id_; }

 private:
  bool is_valid_;
  uint32_t node_id_ = 0;
  uint64_t actor_id_ = 0;
  detail::TypeErasedActor* type_erased_actor_ = nullptr;
};

template <ex::scheduler Scheduler>
class ActorRegistry {
 public:
  explicit ActorRegistry(Scheduler scheduler) : scheduler_(std::move(scheduler)) {
    std::random_device rd;
    random_num_generator_ = std::mt19937(rd());
  }

  ~ActorRegistry() {
    std::scoped_lock locker(mu_);
    // bulk destroy actors
    auto destroy_msg = std::make_unique<detail::DestroyMessage>();
    for (auto& [_, actor] : actor_id_to_actor_) {
      actor->PushMessage(destroy_msg.get(), /*mailbox_partition_index=*/0);
    }
    actor_id_to_actor_.clear();
  }

  /**
   * @brief Create an actor with a manually specified config.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> CreateActor(ActorConfig&& config, Args&&... args) {
    std::scoped_lock locker(mu_);
    auto actor_id = GenerateRandomActorId();
    if (config.actor_name.has_value()) {
      actor_name_to_actor_id_[*config.actor_name] = actor_id;
    }
    auto actor = std::make_unique<detail::Actor<UserClass, Scheduler>>(scheduler_, std::move(config),
                                                                       std::forward<Args>(args)...);
    auto handle = ActorRef<UserClass>(this_node_id_, actor_id, actor.get());
    actor_id_to_actor_[actor_id] = std::move(actor);
    return handle;
  }

  /**
   * @brief Create actor at current node using default config.
   */
  template <class UserClass, class... Args>
  ActorRef<UserClass> CreateActor(Args&&... args) {
    return CreateActor<UserClass>(ActorConfig {.node_id = this_node_id_}, std::forward<Args>(args)...);
  }

  template <class UserClass>
  ActorRef<UserClass> GetActorByName(const std::string& actor_name) {
    std::scoped_lock locker(mu_);
    if (!actor_name_to_actor_id_.contains(actor_name)) {
      throw std::runtime_error("Actor with name " + actor_name + " not found");
    }
    auto actor_id = actor_name_to_actor_id_.at(actor_name);
    return ActorRef<UserClass>(this_node_id_, actor_id, actor_id_to_actor_.at(actor_id).get());
  }

  template <class UserClass>
  void DestroyActor(const ActorRef<UserClass>& actor_ref) {
    std::scoped_lock locker(mu_);
    auto actor_id = actor_ref.GetActorId();
    if (!actor_id_to_actor_.contains(actor_id)) {
      throw std::runtime_error("Actor with id " + std::to_string(actor_id) + " not found");
    }
    auto& actor = actor_id_to_actor_.at(actor_id);
    if (auto actor_name = actor->GetActorName(); actor_name.has_value()) {
      actor_name_to_actor_id_.erase(actor_name.value());
    }
    actor_id_to_actor_.erase(actor_id);
  }

 private:
  uint64_t GenerateRandomActorId() {
    while (true) {
      auto id = random_num_generator_();
      if (!actor_id_to_actor_.contains(id)) {
        return id;
      }
    }
  }

  Scheduler scheduler_;
  mutable std::mutex mu_;
  std::mt19937 random_num_generator_;
  uint32_t this_node_id_ = 0;
  std::unordered_map<std::string, uint64_t> actor_name_to_actor_id_;
  std::unordered_map<uint64_t, std::unique_ptr<detail::TypeErasedActor>> actor_id_to_actor_;
};
}  // namespace ex_actor

namespace std {
template <class UserClass>
struct hash<ex_actor::ActorRef<UserClass>> {
  size_t operator()(const ex_actor::ActorRef<UserClass>& ref) const {
    if (!ref.IsValid()) {
      return 0;
    }
    return std::hash<uint64_t>()(ref.GetActorId()) ^ std::hash<uint32_t>()(ref.GetNodeId());
  }
};
}  // namespace std