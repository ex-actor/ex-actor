#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/internal/util.h"

namespace ex_actor {
namespace {

TEST(PreallocSlotAllocatorTest, ReusesSlotAndFallsBackWhileOccupied) {
  PreallocSlot<512> storage;
  PreallocSlotAllocator<uint64_t, 512> allocator(&storage);
  auto* first = allocator.allocate(/*count=*/1);
  auto* overlapping = allocator.allocate(/*count=*/1);
  EXPECT_NE(first, overlapping);
  *first = 17;
  *overlapping = 29;
  EXPECT_EQ(*first, 17);
  allocator.deallocate(overlapping, /*count=*/1);
  // Freeing a fallback allocation must not release the occupied inline slot.
  EXPECT_EQ(storage.TryAllocate(sizeof(uint64_t), alignof(uint64_t)), nullptr);
  allocator.deallocate(first, /*count=*/1);
  auto* reused = allocator.allocate(/*count=*/1);
  EXPECT_EQ(reused, first);
  allocator.deallocate(reused, /*count=*/1);
}

TEST(PreallocSlotAllocatorTest, FallsBackForOversizedOveralignedAndMultipleObjects) {
  constexpr size_t kSlotSize = 64;
  struct Large {
    std::byte data[kSlotSize + 1];
  };
  struct alignas(2 * alignof(std::max_align_t)) Overaligned {
    uint64_t value;
  };
  PreallocSlot<kSlotSize> storage;
  PreallocSlotAllocator<Large, kSlotSize> large_allocator(&storage);
  PreallocSlotAllocator<Overaligned, kSlotSize> aligned_allocator(&storage);
  PreallocSlotAllocator<uint64_t, kSlotSize> array_allocator(&storage);
  auto* large = large_allocator.allocate(/*count=*/1);
  auto* aligned = aligned_allocator.allocate(/*count=*/1);
  auto* array = array_allocator.allocate(/*count=*/2);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(aligned) % alignof(Overaligned), 0);
  array[0] = 11;
  array[1] = 13;
  EXPECT_EQ(array[0] + array[1], 24);
  // All three requests should leave the inline slot available.
  auto* slot = storage.TryAllocate(sizeof(uint64_t), alignof(uint64_t));
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(storage.TryDeallocate(slot));
  large_allocator.deallocate(large, /*count=*/1);
  aligned_allocator.deallocate(aligned, /*count=*/1);
  array_allocator.deallocate(array, /*count=*/2);
}

TEST(PreallocSlotAllocatorTest, ReboundAllocatorSharesStorage) {
  constexpr size_t kSlotSize = 64;
  PreallocSlot<kSlotSize> storage;
  PreallocSlotAllocator<std::byte, kSlotSize> bytes(&storage);
  std::allocator_traits<decltype(bytes)>::rebind_alloc<uint64_t> words(bytes);
  EXPECT_EQ(bytes, words);
  auto* word = words.allocate(/*count=*/1);
  EXPECT_EQ(storage.TryAllocate(sizeof(uint64_t), alignof(uint64_t)), nullptr);
  words.deallocate(word, /*count=*/1);
  auto* byte = bytes.allocate(/*count=*/1);
  EXPECT_EQ(static_cast<void*>(word), static_cast<void*>(byte));
  bytes.deallocate(byte, /*count=*/1);
}

TEST(PreallocSlotAllocatorTest, CanDeallocateOnAnotherThread) {
  PreallocSlot<512> storage;
  PreallocSlotAllocator<uint64_t, 512> allocator(&storage);
  auto* first = allocator.allocate(/*count=*/1);
  std::thread worker([allocator, first]() mutable { allocator.deallocate(first, /*count=*/1); });
  worker.join();
  auto* reused = allocator.allocate(/*count=*/1);
  EXPECT_EQ(first, reused);
  allocator.deallocate(reused, /*count=*/1);
}

TEST(PreallocSlotAllocatorTest, ConcurrentRequestsNeverShareLiveStorage) {
  constexpr size_t kThreads = 8;
  PreallocSlot<512> storage;
  std::barrier barrier(kThreads);
  std::vector<std::jthread> workers;
  workers.reserve(kThreads);
  const auto start = std::chrono::steady_clock::now();
  for (size_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&, thread, start] {
      PreallocSlotAllocator<uint64_t, 512> allocator(&storage);
      for (uint64_t round = 0; round < 1000; ++round) {
        auto* value = allocator.allocate(/*count=*/1);
        const auto expected = (round * kThreads) + thread;
        *value = expected;
        barrier.arrive_and_wait();
        EXPECT_EQ(*value, expected);
        barrier.arrive_and_wait();
        allocator.deallocate(value, /*count=*/1);
        barrier.arrive_and_wait();
        if (thread == 0 && (round + 1) % 100 == 0) {
          const auto elapsed = std::chrono::steady_clock::now() - start;
          std::cerr << "Completed " << round + 1 << " allocation rounds in "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
        }
      }
    });
  }
}

}  // namespace
}  // namespace ex_actor
