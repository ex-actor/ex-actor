#include <cstdint>

#include <gtest/gtest.h>

#include "ex_actor/api.h"
#include "ex_actor/internal/serialization.h"

namespace {

class Counter {
 public:
  void Add(int x) { count_ += x; }
  int GetValue() const { return count_; }

 private:
  int count_ = 0;
};

struct ActorRefWrapper {
  ex_actor::ActorRef<Counter> ref;
};

}  // namespace

TEST(SerializationTest, EmptyActorRefRemainsEmptyAfterSerializeRoundTrip) {
  ActorRefWrapper wrapper {.ref = {}};
  ASSERT_TRUE(wrapper.ref.IsEmpty());

  auto serialized = ex_actor::internal::Serialize(wrapper);
  auto deserialized = ex_actor::internal::Deserialize<ActorRefWrapper>(serialized);
  EXPECT_TRUE(deserialized.ref.IsEmpty());
}

TEST(SerializationTest, NonEmptyActorRefRemainsNonEmptyAfterSerializeRoundTrip) {
  ex_actor::ActorRef<Counter> ref(
      /*this_node_id=*/1, /*node_id=*/2, /*actor_id=*/42, /*actor=*/nullptr, /*broker_actor_ref=*/ {});
  ActorRefWrapper wrapper {.ref = ref};
  ASSERT_FALSE(wrapper.ref.IsEmpty());

  auto serialized = ex_actor::internal::Serialize(wrapper);
  auto deserialized = ex_actor::internal::Deserialize<ActorRefWrapper>(serialized);
  EXPECT_FALSE(deserialized.ref.IsEmpty());
  EXPECT_EQ(deserialized.ref.GetNodeId(), 2);
  EXPECT_EQ(deserialized.ref.GetActorId(), 42);
}
