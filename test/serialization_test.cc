#include "ex_actor/internal/serialization.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

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

// CTest supplies the expected value separately from the process environment.
uint64_t expected_traversal_limit_in_words = 64ULL * 1024 * 1024;

struct Payload {
  ex_actor::internal::ByteBuffer data;
};

struct EmptyContext {};

class TraversalLimitTest : public testing::TestWithParam<bool> {
 protected:
  Payload Read(const ex_actor::internal::ByteBuffer& serialized) {
    if (GetParam()) {
      return ex_actor::internal::Deserialize<Payload>(serialized, EmptyContext {});
    }
    return ex_actor::internal::Deserialize<Payload>(serialized);
  }
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

TEST_P(TraversalLimitTest, DefaultAllowsMessageLargerThan64MiB) {
  const Payload payload {.data = ex_actor::internal::ByteBuffer(72 * 1024 * 1024, std::byte {0x5a})};
  const auto serialized = ex_actor::internal::Serialize(payload);

  // Reproduce the original failure using Cap'n Proto's default 64 MiB limit.
  const auto bytes =
      kj::ArrayPtr<const kj::byte>(reinterpret_cast<const kj::byte*>(serialized.data()), serialized.size());
  kj::ArrayInputStream input_stream(bytes);
  EXPECT_THROW({ capnp::PackedMessageReader reader(input_stream); }, kj::Exception);

  EXPECT_EQ(ex_actor::internal::GetCapnpReaderOptions().traversalLimitInWords, 64ULL * 1024 * 1024);
  EXPECT_EQ(Read(serialized).data, payload.data);
}

TEST_P(TraversalLimitTest, EnvironmentOverrideControlsBothReaders) {
  const Payload payload {.data = ex_actor::internal::ByteBuffer(4096, std::byte {0x5a})};
  const auto serialized = ex_actor::internal::Serialize(payload);

  EXPECT_EQ(ex_actor::internal::GetCapnpReaderOptions().traversalLimitInWords, expected_traversal_limit_in_words);
  if (expected_traversal_limit_in_words == 64) {
    // 512 bytes: too small for this message.
    EXPECT_THROW(Read(serialized), kj::Exception);
  } else {
    // Both the 32 KiB override and the default allow this message.
    EXPECT_EQ(Read(serialized).data, payload.data);
  }
}

INSTANTIATE_TEST_SUITE_P(Serialization, TraversalLimitTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                           return info.param ? "WithContext" : "WithoutContext";
                         });

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  if (argc == 2) {
    expected_traversal_limit_in_words = std::stoull(argv[1]);
  } else if (argc != 1) {
    return 1;
  }
  return RUN_ALL_TESTS();
}
