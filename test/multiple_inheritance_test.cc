// Copyright 2025 The ex_actor Authors.
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

#include <barrier>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <exec/task.hpp>
#include <gtest/gtest.h>
#include <stdexec/execution.hpp>

#include "ex_actor/api.h"

using namespace ex_actor;

namespace {

// ==================== Test Hierarchy ====================

class Animal {
 public:
  virtual ~Animal() = default;
  virtual std::string Speak() { return "sound"; }
};

class Dog : public Animal {
 public:
  static Dog Create() { return Dog(); }
  std::string Speak() override { return "woof"; }
};

class Cat : public Animal {
 public:
  static Cat Create() { return Cat(); }
  std::string Speak() override { return "meow"; }
};

/**
 * Multiple Inheritance hierarchy
 */
struct BaseA {
  virtual ~BaseA() = default;
  virtual int GetA() { return 1; }
};

struct BaseB {
  virtual ~BaseB() = default;
  virtual int GetB() { return 2; }
};

struct Derived : public BaseA, public BaseB {
  static Derived Create() { return Derived(); }
  int GetA() override { return 10; }
  int GetB() override { return 20; }
};

/**
 * Partial Registration hierarchy:
 * Used to demonstrate that remote calls require explicit registration via EXA_REMOTE,
 * while local calls do not.
 */
struct BasePartialA {
  virtual ~BasePartialA() = default;
  virtual int GetA() { return 100; }
};

struct BasePartialB {
  virtual ~BasePartialB() = default;
  virtual int GetB() { return 200; }
};

struct DerivedPartial : public BasePartialA, public BasePartialB {
  static DerivedPartial Create() { return DerivedPartial(); }
};

class Swimmer {
 public:
  virtual ~Swimmer() = default;
  virtual std::string Swim() { return "swimming"; }
};

class Duck : public Animal, public Swimmer {
 public:
  static Duck Create() { return Duck(); }
  std::string Speak() override { return "quack"; }
  std::string Swim() override { return "paddling"; }
};

// ==================== Slicing Hierarchy ====================

struct SlicedBase {
  virtual ~SlicedBase() = default;
  virtual std::string GetType() { return "Base"; }
};

struct SlicedDerived : public SlicedBase {
  // This factory returns by value as SlicedBase, causing slicing inside the factory.
  static SlicedBase CreateSlicing() { return SlicedDerived(); }
  static SlicedDerived CreateCorrect() { return SlicedDerived(); }
  std::string GetType() override { return "Derived"; }
};

// Register remote handlers
EXA_REMOTE(&Dog::Create, &Dog::Speak);
EXA_REMOTE(&Cat::Create, &Cat::Speak);
EXA_REMOTE(&Derived::Create, &BaseA::GetA, &BaseB::GetB);
EXA_REMOTE(&Duck::Create, &Animal::Speak, &Swimmer::Swim);
EXA_REMOTE(&SlicedDerived::CreateSlicing, &SlicedBase::GetType);
EXA_REMOTE(&SlicedDerived::CreateCorrect);

// IMPORTANT: We only register GetA, NOT GetB for DerivedPartial
EXA_REMOTE(&DerivedPartial::Create, &BasePartialA::GetA);

// ==================== Test Helpers ====================

void RunTwoNodeTest(auto test_logic) {
  std::barrier bar {2};
  auto node_main = [&](size_t index) -> exec::task<void> {
    std::vector<std::string> addresses = {"tcp://127.0.0.1:5503", "tcp://127.0.0.1:5504"};
    ClusterConfig config {
        .listen_address = addresses.at(index),
        .contact_node_address = (index == 0) ? "" : addresses.at(0),
    };
    ActorRegistry registry(/*thread_pool_size=*/2);
    co_await registry.StartOrJoinCluster(config);

    auto [state, condition_met] =
        co_await registry.WaitClusterState([](const auto& s) { return s.nodes.size() >= 2; }, 5000);

    EXPECT_TRUE(condition_met);
    if (condition_met) {
      uint64_t remote_id = 0;
      for (const auto& n : state.nodes) {
        if (n.address == addresses.at(1 - index)) {
          remote_id = n.node_id;
          break;
        }
      }
      co_await test_logic(index, remote_id, registry);
    }
    bar.arrive_and_wait();
    co_return;
  };

  std::jthread n0([&] { stdexec::sync_wait(node_main(0)); });
  std::jthread n1([&] { stdexec::sync_wait(node_main(1)); });
}

}  // namespace

// ==================== Local Mode Tests ====================

class MultipleInheritanceTest : public ::testing::Test {
 protected:
  void SetUp() override { Init(2); }
  void TearDown() override { Shutdown(); }
};

TEST_F(MultipleInheritanceTest, LocalPolymorphismAndOffsetAdjustment) {
  auto test_coro = []() -> exec::task<void> {
    auto dog_ref = co_await Spawn<Dog>();
    ActorRef<Animal> animal_ref = dog_ref;
    EXPECT_EQ(co_await animal_ref.SendLocal<&Animal::Speak>(), "woof");

    auto derived_ref = co_await Spawn<Derived>();
    EXPECT_EQ(co_await derived_ref.Send<&BaseA::GetA>(), 10);
    EXPECT_EQ(co_await derived_ref.Send<&BaseB::GetB>(), 20);

    auto duck_ref = co_await Spawn<Duck>();
    ActorRef<Swimmer> swimmer_ref = duck_ref;
    EXPECT_EQ(co_await swimmer_ref.SendLocal<&Swimmer::Swim>(), "paddling");
    co_return;
  };
  stdexec::sync_wait(test_coro());
}

/**
 * @brief Verifies that local calls work even for methods not registered in EXA_REMOTE.
 */
TEST_F(MultipleInheritanceTest, LocalUnregisteredMethodSucceeds) {
  auto test_coro = []() -> exec::task<void> {
    auto ref = co_await Spawn<DerivedPartial>();
    // GetB is NOT registered in EXA_REMOTE, but SendLocal (and Send in local mode)
    // works because it uses direct memory access rather than handler lookup.
    EXPECT_EQ(co_await ref.Send<&BasePartialB::GetB>(), 200);
    co_return;
  };
  stdexec::sync_wait(test_coro());
}

/**
 * @brief the framework prevents and detects object slicing.
 */
TEST_F(MultipleInheritanceTest, SlicingDetectionAndPrevention) {
  auto test_coro = []() -> exec::task<void> {
    // 1. If a factory returns SlicedBase by value, slicing happens immediately.
    // The framework correctly creates an Actor<SlicedBase>.
    auto base_ref = co_await Spawn<&SlicedDerived::CreateSlicing>();
    EXPECT_EQ(co_await base_ref.Send<&SlicedBase::GetType>(), "Base");

    // 2. Demonstrate compile-time type safety:
    // ActorRef<Base> returned by Spawn<&CreateSlicing> cannot be assigned to ActorRef<Derived>.
    using BaseRef = ActorRef<SlicedBase>;
    using DerivedRef = ActorRef<SlicedDerived>;
    using SpawnResultTuple = std::decay_t<decltype(stdexec::sync_wait(Spawn<&SlicedDerived::CreateSlicing>()).value())>;
    using SpawnedActorRef = std::tuple_element_t<0, SpawnResultTuple>;

    static_assert(std::is_same_v<SpawnedActorRef, BaseRef>,
                  "Spawn should return ActorRef<Base> for a factory returning Base");

    static_assert(!std::is_convertible_v<BaseRef, DerivedRef>,
                  "ActorRef<Base> should not be convertible to ActorRef<Derived> (no implicit downcast)");

    // 3. Correct usage preserves polymorphism:
    auto derived_ref = co_await Spawn<&SlicedDerived::CreateCorrect>();
    EXPECT_EQ(co_await derived_ref.Send<&SlicedBase::GetType>(), "Derived");

    co_return;
  };
  stdexec::sync_wait(test_coro());
}

// ==================== Distributed Mode Tests ====================

TEST(MultipleInheritanceRemoteTest, DistributedPolymorphismAndOffsetAdjustment) {
  RunTwoNodeTest([](size_t index, uint64_t remote_id, ActorRegistry& registry) -> exec::task<void> {
    if (index == 0) {
      auto actor = co_await registry.Spawn<&Derived::Create>().ToNode(remote_id);
      EXPECT_EQ(co_await actor.Send<&BaseA::GetA>(), 10);
      EXPECT_EQ(co_await actor.Send<&BaseB::GetB>(), 20);

      auto duck = co_await registry.Spawn<&Duck::Create>().ToNode(remote_id);
      ActorRef<Swimmer> swimmer = duck;
      EXPECT_EQ(co_await swimmer.Send<&Swimmer::Swim>(), "paddling");
    }
    co_return;
  });
}

/**
 * @brief Verifies that remote calls fail if the method is not registered in EXA_REMOTE.
 */
TEST(MultipleInheritanceRemoteTest, RemoteUnregisteredMethodFails) {
  RunTwoNodeTest([](size_t index, uint64_t remote_id, ActorRegistry& registry) -> exec::task<void> {
    if (index == 0) {
      auto actor = co_await registry.Spawn<&DerivedPartial::Create>().ToNode(remote_id);

      // GetA was registered, should work.
      EXPECT_EQ(co_await actor.Send<&BasePartialA::GetA>(), 100);

      // GetB was NOT registered. The remote node won't find the handler.
      // This should throw an exception.
      bool caught = false;
      try {
        co_await actor.Send<&BasePartialB::GetB>();
      } catch (const std::exception& e) {
        caught = true;
        EXPECT_TRUE(std::string(e.what()).find("Remote actor method call handler not found") != std::string::npos);
      }
      EXPECT_TRUE(caught) << "Should have thrown exception for unregistered remote method";
    }
    co_return;
  });
}
