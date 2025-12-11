#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

// ===========================
// Actor Classes for Shuffle-Merge Workflow
// ===========================

/**
 * @brief Reducer actor that aggregates results from multiple workers
 */
class Reducer {
 public:
  void AddResult(int64_t value) {
    sum_ += value;
    count_++;
  }

  int64_t GetSum() const { return sum_; }
  int GetCount() const { return count_; }

 private:
  int64_t sum_ = 0;
  int count_ = 0;
};

/**
 * @brief Worker actor that processes data and sends results to a reducer
 */
class Worker {
 public:
  explicit Worker(ex_actor::ActorRef<Reducer> reducer) : reducer_(reducer) {}

  // Process a chunk of data (sum of squares) and send to reducer
  exec::task<void> ProcessData(std::vector<int> data) {
    int64_t result = 0;
    for (int val : data) {
      result += static_cast<int64_t>(val) * val;  // compute sum of squares
    }

    // Send result to reducer
    co_await reducer_.template Send<&Reducer::AddResult>(result);
  }

  // Process single value
  exec::task<int64_t> ProcessValue(int value) {
    int64_t result = static_cast<int64_t>(value) * value;
    co_await reducer_.template Send<&Reducer::AddResult>(result);
    co_return result;
  }

 private:
  ex_actor::ActorRef<Reducer> reducer_;
};

/**
 * @brief Data source actor that generates data and distributes to workers (shuffle phase)
 */
class DataSource {
 public:
  explicit DataSource(std::vector<ex_actor::ActorRef<Worker>> workers) : workers_(std::move(workers)) {}

  // Generate deterministic data and shuffle to workers
  // Generates sequential numbers starting from start_value
  exec::task<void> GenerateAndShuffleDeterministic(int data_size, int start_value) {
    // Generate deterministic data chunks (sequential numbers)
    std::vector<int> data;
    data.reserve(data_size);
    for (int i = 0; i < data_size; ++i) {
      data.push_back(start_value + i);
    }

    // Shuffle data to workers
    int chunk_size = (data_size + workers_.size() - 1) / workers_.size();
    for (size_t worker_idx = 0; worker_idx < workers_.size(); ++worker_idx) {
      int start = worker_idx * chunk_size;
      int end = std::min(start + chunk_size, data_size);

      if (start < end) {
        std::vector<int> chunk(data.begin() + start, data.begin() + end);
        co_await workers_[worker_idx].template Send<&Worker::ProcessData>(std::move(chunk));
      }
    }
  }

  // Send individual values round-robin to workers
  exec::task<void> DistributeValues(std::vector<int> values) {
    for (size_t i = 0; i < values.size(); ++i) {
      size_t worker_idx = i % workers_.size();
      co_await workers_[worker_idx].template Send<&Worker::ProcessValue>(values[i]);
    }
  }

 private:
  std::vector<ex_actor::ActorRef<Worker>> workers_;
};

/**
 * @brief Coordinator actor that manages the entire workflow
 */
class Coordinator {
 public:
  Coordinator(std::vector<ex_actor::ActorRef<DataSource>> sources, std::vector<ex_actor::ActorRef<Reducer>> reducers)
      : sources_(std::move(sources)), reducers_(std::move(reducers)) {}

  // Execute the entire shuffle-merge workflow with deterministic data
  exec::task<void> ExecuteWorkflow(int data_size_per_source) {
    exec::async_scope scope;

    // Launch all data sources in parallel with deterministic sequential data
    // Each source gets an unique range of sequential numbers
    for (size_t i = 0; i < sources_.size(); ++i) {
      int start_value = static_cast<int>((i * data_size_per_source) + 1);
      scope.spawn(
          sources_[i].template Send<&DataSource::GenerateAndShuffleDeterministic>(data_size_per_source, start_value));
    }

    // Wait for all work to complete
    co_await scope.on_empty();
  }

  // Collect results from all reducers
  exec::task<std::vector<int64_t>> CollectResults() {
    std::vector<int64_t> results;
    results.reserve(reducers_.size());
    for (auto& reducer : reducers_) {
      int64_t sum = co_await reducer.template Send<&Reducer::GetSum>();
      results.push_back(sum);
    }
    co_return results;
  }

 private:
  std::vector<ex_actor::ActorRef<DataSource>> sources_;
  std::vector<ex_actor::ActorRef<Reducer>> reducers_;
};

// ===========================
// Test Cases
// ===========================

TEST(ShuffleMergeTest, BasicShuffleMergeWorkflow) {
  auto coroutine = []() -> exec::task<void> {
    // Create thread pool with multiple threads
    ex_actor::WorkSharingThreadPool thread_pool(8);
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

    constexpr int kNumReducers = 4;
    constexpr int kNumWorkers = 16;
    constexpr int kNumSources = 8;
    constexpr int kDataSizePerSource = 1000;

    // Create reducers
    std::vector<ex_actor::ActorRef<Reducer>> reducers;
    reducers.reserve(kNumReducers);
    for (int i = 0; i < kNumReducers; ++i) {
      reducers.push_back(co_await registry.CreateActor<Reducer>());
    }

    // Create workers, each connected to a reducer (round-robin)
    std::vector<ex_actor::ActorRef<Worker>> workers;
    workers.reserve(kNumWorkers);
    for (int i = 0; i < kNumWorkers; ++i) {
      workers.push_back(co_await registry.CreateActor<Worker>(reducers[i % kNumReducers]));
    }

    // Create data sources, each with reference to all workers
    std::vector<ex_actor::ActorRef<DataSource>> sources;
    sources.reserve(kNumSources);
    for (int i = 0; i < kNumSources; ++i) {
      sources.push_back(co_await registry.CreateActor<DataSource>(workers));
    }

    // Create coordinator
    auto coordinator = co_await registry.CreateActor<Coordinator>(sources, reducers);
    co_await coordinator.template Send<&Coordinator::ExecuteWorkflow>(kDataSizePerSource);

    // Collect and verify results
    auto results = co_await coordinator.template Send<&Coordinator::CollectResults>();

    EXPECT_EQ(results.size(), kNumReducers);

    // Verify all reducers received data
    int64_t total_sum = 0;
    for (int64_t sum : results) {
      EXPECT_GT(sum, 0);
      total_sum += sum;
    }

    // Calculate expected sum: sum of squares from 1 to N where N = kNumSources * kDataSizePerSource
    // Formula: sum of i^2 from 1 to N = N(N+1)(2N+1)/6
    int64_t total_items = kNumSources * kDataSizePerSource;
    int64_t expected_sum = total_items * (total_items + 1) * (2 * total_items + 1) / 6;

    EXPECT_EQ(total_sum, expected_sum) << "Total sum should match formula for sum of squares from 1 to " << total_items;
  };

  ex::sync_wait(coroutine());
}

TEST(ShuffleMergeTest, LargeScaleShuffleMerge) {
  auto coroutine = []() -> exec::task<void> {
    ex_actor::WorkSharingThreadPool thread_pool(16);
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

    constexpr int kNumReducers = 8;
    constexpr int kNumWorkers = 64;
    constexpr int kNumSources = 32;
    constexpr int kDataSizePerSource = 500;

    auto start_time = std::chrono::steady_clock::now();

    // Create actor hierarchy
    std::vector<ex_actor::ActorRef<Reducer>> reducers;
    reducers.reserve(kNumReducers);
    for (int i = 0; i < kNumReducers; ++i) {
      reducers.push_back(co_await registry.CreateActor<Reducer>());
    }

    std::vector<ex_actor::ActorRef<Worker>> workers;
    workers.reserve(kNumWorkers);
    for (int i = 0; i < kNumWorkers; ++i) {
      workers.push_back(co_await registry.CreateActor<Worker>(reducers[i % kNumReducers]));
    }

    std::vector<ex_actor::ActorRef<DataSource>> sources;
    sources.reserve(kNumSources);
    for (int i = 0; i < kNumSources; ++i) {
      sources.push_back(co_await registry.CreateActor<DataSource>(workers));
    }

    auto coordinator = co_await registry.CreateActor<Coordinator>(sources, reducers);
    co_await coordinator.template Send<&Coordinator::ExecuteWorkflow>(kDataSizePerSource);

    auto results = co_await coordinator.template Send<&Coordinator::CollectResults>();

    EXPECT_EQ(results.size(), kNumReducers);

    int64_t total_sum = 0;
    int64_t max_sum = 0;
    int64_t min_sum = INT64_MAX;

    for (int64_t sum : results) {
      EXPECT_GT(sum, 0);
      total_sum += sum;
      max_sum = std::max(max_sum, sum);
      min_sum = std::min(min_sum, sum);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    // Verify workflow completed in reasonable time (less than 5 seconds)
    EXPECT_LT(duration.count(), 5000);

    // Calculate expected sum: sum of squares from 1 to N where N = kNumSources * kDataSizePerSource
    int64_t total_items = kNumSources * kDataSizePerSource;
    int64_t expected_sum = total_items * (total_items + 1) * (2 * total_items + 1) / 6;

    EXPECT_EQ(total_sum, expected_sum) << "Total sum should match formula for sum of squares from 1 to " << total_items;

    // Verify load balance - with deterministic round-robin distribution,
    // load should be reasonably balanced
    double load_balance_ratio = static_cast<double>(max_sum) / min_sum;
    EXPECT_LT(load_balance_ratio, 2.0) << "Load imbalance too high: " << load_balance_ratio;
  };

  ex::sync_wait(coroutine());
}

TEST(ShuffleMergeTest, MultiStageShuffleMerge) {
  auto coroutine = []() -> exec::task<void> {
    // Test with multiple stages of shuffle-merge (like multi-level MapReduce)
    ex_actor::WorkSharingThreadPool thread_pool(12);
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

    constexpr int kNumStage1Reducers = 16;
    constexpr int kNumStage2Reducers = 4;
    constexpr int kNumWorkers = 64;
    constexpr int kNumSources = 16;
    constexpr int kDataSizePerSource = 200;

    // Stage 1: Create first level reducers
    std::vector<ex_actor::ActorRef<Reducer>> stage1_reducers;
    stage1_reducers.reserve(kNumStage1Reducers);
    for (int i = 0; i < kNumStage1Reducers; ++i) {
      stage1_reducers.push_back(co_await registry.CreateActor<Reducer>());
    }

    // Create workers for stage 1
    std::vector<ex_actor::ActorRef<Worker>> workers;
    workers.reserve(kNumWorkers);
    for (int i = 0; i < kNumWorkers; ++i) {
      workers.push_back(co_await registry.CreateActor<Worker>(stage1_reducers[i % kNumStage1Reducers]));
    }

    // Create sources
    std::vector<ex_actor::ActorRef<DataSource>> sources;
    sources.reserve(kNumSources);
    for (int i = 0; i < kNumSources; ++i) {
      sources.push_back(co_await registry.CreateActor<DataSource>(workers));
    }

    // Execute stage 1
    exec::async_scope scope;

    // Launch all sources with deterministic sequential data
    for (size_t i = 0; i < sources.size(); ++i) {
      int start_value = static_cast<int>((i * kDataSizePerSource) + 1);
      scope.spawn(
          sources[i].template Send<&DataSource::GenerateAndShuffleDeterministic>(kDataSizePerSource, start_value));
    }

    co_await scope.on_empty();

    // Collect stage 1 results
    std::vector<int64_t> stage1_results;
    stage1_results.reserve(stage1_reducers.size());
    for (auto& reducer : stage1_reducers) {
      int64_t sum = co_await reducer.template Send<&Reducer::GetSum>();
      int count = co_await reducer.template Send<&Reducer::GetCount>();
      stage1_results.push_back(sum);
      EXPECT_GT(sum, 0) << "Stage 1 reducer should have processed data";
      EXPECT_GT(count, 0) << "Stage 1 reducer should have received items";
    }

    // Verify we got results from all stage 1 reducers
    EXPECT_EQ(stage1_results.size(), kNumStage1Reducers);

    // Stage 2: Create second level reducers
    std::vector<ex_actor::ActorRef<Reducer>> stage2_reducers;
    stage2_reducers.reserve(kNumStage2Reducers);
    for (int i = 0; i < kNumStage2Reducers; ++i) {
      stage2_reducers.push_back(co_await registry.CreateActor<Reducer>());
    }

    // Create workers for stage 2 to aggregate stage 1 results
    std::vector<ex_actor::ActorRef<Worker>> stage2_workers;
    stage2_workers.reserve(kNumStage2Reducers);
    for (int i = 0; i < kNumStage2Reducers; ++i) {
      stage2_workers.push_back(co_await registry.CreateActor<Worker>(stage2_reducers[i]));
    }

    // Distribute stage 1 results to stage 2
    for (size_t i = 0; i < stage1_results.size(); ++i) {
      size_t worker_idx = i % stage2_workers.size();
      co_await stage2_workers[worker_idx].template Send<&Worker::ProcessValue>(stage1_results[i]);
    }

    // Collect final results
    int64_t final_sum = 0;
    int total_stage2_count = 0;
    for (auto& reducer : stage2_reducers) {
      int64_t sum = co_await reducer.template Send<&Reducer::GetSum>();
      int count = co_await reducer.template Send<&Reducer::GetCount>();
      final_sum += sum;
      total_stage2_count += count;
      EXPECT_GT(sum, 0) << "Stage 2 reducer should have processed data";
      EXPECT_EQ(count, kNumStage1Reducers / kNumStage2Reducers) << "Stage 2 reducers should receive equal items";
    }

    // Verify stage 2 processed all stage 1 results
    EXPECT_EQ(total_stage2_count, kNumStage1Reducers) << "Stage 2 should process all stage 1 results";

    // Stage 2 computes squares of stage 1 sums
    // Calculate expected final sum: sum of (stage1_sum^2) for all stage1 results
    int64_t expected_final_sum = 0;
    for (int64_t stage1_sum : stage1_results) {
      expected_final_sum += stage1_sum * stage1_sum;
    }

    EXPECT_EQ(final_sum, expected_final_sum) << "Stage 2 final sum should be sum of squares of stage 1 results";
  };

  ex::sync_wait(coroutine());
}

TEST(ShuffleMergeTest, ComplexDependencyGraph) {
  auto coroutine = []() -> exec::task<void> {
    // Test with complex dependency graph where workers communicate with multiple reducers
    ex_actor::WorkSharingThreadPool thread_pool(8);
    ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

    constexpr int kNumReducers = 8;
    constexpr int kNumValues = 10000;

    // Create reducers
    std::vector<ex_actor::ActorRef<Reducer>> reducers;
    reducers.reserve(kNumReducers);
    for (int i = 0; i < kNumReducers; ++i) {
      reducers.push_back(co_await registry.CreateActor<Reducer>());
    }

    // Create workers, each connected to a different reducer
    std::vector<ex_actor::ActorRef<Worker>> workers;
    workers.reserve(kNumReducers);
    for (int i = 0; i < kNumReducers; ++i) {
      workers.push_back(co_await registry.CreateActor<Worker>(reducers[i]));
    }

    // Create a single source that will distribute to all workers
    auto source = co_await registry.CreateActor<DataSource>(workers);
    // Generate values 1 to kNumValues
    std::vector<int> values;
    values.reserve(kNumValues);
    for (int i = 1; i <= kNumValues; ++i) {
      values.push_back(i);
    }

    co_await source.template Send<&DataSource::DistributeValues>(std::move(values));

    // Each reducer should receive approximately equal number of values
    std::vector<int> counts;
    int64_t total_sum = 0;

    for (auto& reducer : reducers) {
      int count = co_await reducer.template Send<&Reducer::GetCount>();
      int64_t sum = co_await reducer.template Send<&Reducer::GetSum>();
      counts.push_back(count);
      total_sum += sum;
      EXPECT_GT(sum, 0) << "Each reducer should have processed data";
    }

    // Verify load balance (each reducer should get kNumValues/kNumReducers items)
    int expected_count_per_reducer = kNumValues / kNumReducers;
    for (size_t i = 0; i < counts.size(); ++i) {
      EXPECT_EQ(counts[i], expected_count_per_reducer)
          << "Reducer " << i << " should receive exactly " << expected_count_per_reducer << " items";
    }

    // Verify total: sum of squares from 1 to N = N(N+1)(2N+1)/6
    int64_t expected_sum = static_cast<int64_t>(kNumValues) * (kNumValues + 1) * (2 * kNumValues + 1) / 6;
    EXPECT_EQ(total_sum, expected_sum) << "Total sum should match mathematical formula for sum of squares";
  };

  ex::sync_wait(coroutine());
}
