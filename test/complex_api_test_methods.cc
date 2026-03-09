#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

// ===========================
// Actor 6: Methods with Various Argument Types
// ===========================

class VariousArgsActor {
 public:
  VariousArgsActor() = default;

  // No arguments
  int GetState() const { return state_; }

  // Single argument - primitive
  void SetState(int state) { state_ = state; }

  // Two double arguments to compute distance
  void ProcessCoordinates(double x, double y) { last_distance_ = std::sqrt((x * x) + (y * y)); }

  double GetLastDistance() const { return last_distance_; }

  // Multiple arguments - primitives
  int Add(int a, int b) {
    int result = a + b;
    state_ += result;
    return result;
  }

  // Multiple arguments - mixed types
  std::string FormatMessage(int id, const std::string& prefix, double value) {
    return prefix + std::to_string(id) + ": " + std::to_string(value);
  }

  // Argument - three separate values instead of struct
  void ProcessDataFields(std::string name, int value, std::vector<double> data) {
    record_names_.push_back(std::move(name));
    record_values_.push_back(value);
    record_data_.push_back(std::move(data));
  }

  size_t GetRecordCount() const { return record_names_.size(); }

  std::string GetRecordName(size_t index) const { return index < record_names_.size() ? record_names_[index] : ""; }

  // Argument - vector
  int SumVector(const std::vector<int>& values) {
    int sum = 0;
    for (int v : values) {
      sum += v;
    }
    return sum;
  }

  // Multiple arguments - containers
  void UpdateMaps(std::map<std::string, int> m1, std::map<int, std::string> m2) {
    map1_ = std::move(m1);
    map2_ = std::move(m2);
  }

  size_t GetMapSizes() const { return map1_.size() + map2_.size(); }

  // Complex nested containers
  void ProcessNestedData(std::vector<std::vector<int>> nested, std::map<std::string, std::vector<int>> mapped) {
    nested_data_ = std::move(nested);
    mapped_data_ = std::move(mapped);
  }

  size_t GetNestedDataSize() const {
    size_t total = 0;
    for (const auto& vec : nested_data_) {
      total += vec.size();
    }
    return total;
  }

 private:
  int state_ = 0;
  double last_distance_ = 0.0;
  std::vector<std::string> record_names_;
  std::vector<int> record_values_;
  std::vector<std::vector<double>> record_data_;
  std::map<std::string, int> map1_;
  std::map<int, std::string> map2_;
  std::vector<std::vector<int>> nested_data_;
  std::map<std::string, std::vector<int>> mapped_data_;
};

// ===========================
// Actor 7: Methods with Move-only Arguments
// ===========================

class MoveOnlyArgsActor {
 public:
  MoveOnlyArgsActor() = default;

  // unique_ptr<int> argument
  void StoreIntPtr(std::unique_ptr<int> ptr) {
    last_value_ = ptr ? *ptr : -1;
    int_ptr_ = std::move(ptr);
  }

  int GetLastValue() const { return last_value_; }

  // unique_ptr<string> argument
  void StoreString(std::unique_ptr<std::string> str) { stored_string_ = std::move(str); }

  std::string GetStoredString() const { return stored_string_ ? *stored_string_ : "null"; }

  // unique_ptr<vector> argument
  size_t ProcessVectorPtr(std::unique_ptr<std::vector<int>> vec_ptr) {
    size_t size = vec_ptr ? vec_ptr->size() : 0;
    vector_ptr_ = std::move(vec_ptr);
    return size;
  }

  bool HasVectorPtr() const { return vector_ptr_ != nullptr; }

  // Multiple unique_ptr arguments
  void StoreMultiple(std::unique_ptr<int> a, std::unique_ptr<double> b) {
    int_ptr_ = std::move(a);
    double_ptr_ = std::move(b);
  }

  std::pair<int, double> GetStoredValues() const {
    int i = int_ptr_ ? *int_ptr_ : 0;
    double d = double_ptr_ ? *double_ptr_ : 0.0;
    return {i, d};
  }

  // unique_ptr to map
  void StoreMap(std::unique_ptr<std::map<std::string, int>> map_ptr) { map_ptr_ = std::move(map_ptr); }

  size_t GetMapSize() const { return map_ptr_ ? map_ptr_->size() : 0; }

 private:
  int last_value_ = 0;
  std::unique_ptr<int> int_ptr_;
  std::unique_ptr<double> double_ptr_;
  std::unique_ptr<std::string> stored_string_;
  std::unique_ptr<std::vector<int>> vector_ptr_;
  std::unique_ptr<std::map<std::string, int>> map_ptr_;
};

// ===========================
// Actor 8: Methods with Reference Return Types
// ===========================

class ReferenceReturnActor {
 public:
  explicit ReferenceReturnActor(std::string initial_data) : data_(std::move(initial_data)) {}

  // Return by value
  std::string GetDataCopy() const { return data_; }

  // Modify data
  void AppendData(const std::string& suffix) { data_ += suffix; }

  void SetData(std::string new_data) { data_ = std::move(new_data); }

  size_t GetDataSize() const { return data_.size(); }

 private:
  std::string data_;
};

// ===========================
// Actor 9: Actor with ActorRef Members
// ===========================

class ChildActor {
 public:
  ChildActor() = default;

  void IncrementCounter() { counter_++; }

  int GetCounter() const { return counter_; }

  void AddValue(int value) { counter_ += value; }

 private:
  int counter_ = 0;
};

class ParentActor {
 public:
  explicit ParentActor(ex_actor::ActorRef<ChildActor> child) : child_(child) {}

  exec::task<void> DelegateIncrement() { co_await child_.template Send<&ChildActor::IncrementCounter>(); }

  exec::task<int> GetChildCounter() { co_return co_await child_.template Send<&ChildActor::GetCounter>(); }

  exec::task<void> AddToChild(int value) { co_await child_.template Send<&ChildActor::AddValue>(value); }

 private:
  ex_actor::ActorRef<ChildActor> child_;
};

// ===========================
// Actor 10: Actor with Multiple ActorRef Members
// ===========================

class Accumulator {
 public:
  Accumulator() = default;

  void Add(int value) { sum_ += value; }

  int GetSum() const { return sum_; }

 private:
  int sum_ = 0;
};

class Distributor {
 public:
  Distributor(ex_actor::ActorRef<Accumulator> acc1, ex_actor::ActorRef<Accumulator> acc2,
              ex_actor::ActorRef<Accumulator> acc3)
      : accumulators_ {acc1, acc2, acc3} {}

  exec::task<void> DistributeValue(int value) {
    // Distribute to all accumulators
    for (auto& acc : accumulators_) {
      co_await acc.template Send<&Accumulator::Add>(value);
    }
  }

  exec::task<std::vector<int>> CollectSums() {
    std::vector<int> sums;
    sums.reserve(accumulators_.size());
    for (auto& acc : accumulators_) {
      int sum = co_await acc.template Send<&Accumulator::GetSum>();
      sums.push_back(sum);
    }
    co_return sums;
  }

 private:
  std::vector<ex_actor::ActorRef<Accumulator>> accumulators_;
};

// ===========================
// Test Cases
// ===========================

TEST(ComplexApiTest, VariousArgumentTypes) {
  auto coroutine = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<VariousArgsActor>();
    // No arguments
    int state = co_await actor.template Send<&VariousArgsActor::GetState>();
    EXPECT_EQ(state, 0);

    // Single primitive
    co_await actor.template Send<&VariousArgsActor::SetState>(10);
    state = co_await actor.template Send<&VariousArgsActor::GetState>();
    EXPECT_EQ(state, 10);

    // Two double arguments
    co_await actor.template Send<&VariousArgsActor::ProcessCoordinates>(3.0, 4.0);
    double distance = co_await actor.template Send<&VariousArgsActor::GetLastDistance>();
    EXPECT_DOUBLE_EQ(distance, 5.0);

    // Multiple primitives
    int sum = co_await actor.template Send<&VariousArgsActor::Add>(7, 8);
    EXPECT_EQ(sum, 15);
    state = co_await actor.template Send<&VariousArgsActor::GetState>();
    EXPECT_EQ(state, 25);  // 10 + 15

    // Mixed types
    std::string msg = co_await actor.template Send<&VariousArgsActor::FormatMessage>(42, "ID:", 3.14);
    EXPECT_FALSE(msg.empty());
    EXPECT_NE(msg.find("ID:42"), std::string::npos);

    // Multiple arguments with containers
    std::vector<double> data = {1.1, 2.2, 3.3};
    co_await actor.template Send<&VariousArgsActor::ProcessDataFields>("test", 100, std::move(data));
    size_t count = co_await actor.template Send<&VariousArgsActor::GetRecordCount>();
    EXPECT_EQ(count, 1);
    std::string name = co_await actor.template Send<&VariousArgsActor::GetRecordName>(0);
    EXPECT_EQ(name, "test");

    // Vector
    std::vector<int> values = {1, 2, 3, 4, 5};
    int vector_sum = co_await actor.template Send<&VariousArgsActor::SumVector>(std::move(values));
    EXPECT_EQ(vector_sum, 15);

    // Multiple containers
    std::map<std::string, int> m1 = {{"x", 1}, {"y", 2}};
    std::map<int, std::string> m2 = {{1, "one"}, {2, "two"}};
    co_await actor.template Send<&VariousArgsActor::UpdateMaps>(std::move(m1), std::move(m2));
    size_t map_sizes = co_await actor.template Send<&VariousArgsActor::GetMapSizes>();
    EXPECT_EQ(map_sizes, 4);

    // Nested containers
    std::vector<std::vector<int>> nested = {{1, 2}, {3, 4, 5}, {6}};
    std::map<std::string, std::vector<int>> mapped = {{"a", {1, 2}}, {"b", {3}}};
    co_await actor.template Send<&VariousArgsActor::ProcessNestedData>(std::move(nested), std::move(mapped));
    size_t nested_size = co_await actor.template Send<&VariousArgsActor::GetNestedDataSize>();
    EXPECT_EQ(nested_size, 6);  // 2 + 3 + 1
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, MoveOnlyArguments) {
  auto coroutine = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<MoveOnlyArgsActor>();
    // unique_ptr<int>
    auto int_ptr = std::make_unique<int>(42);
    co_await actor.template Send<&MoveOnlyArgsActor::StoreIntPtr>(std::move(int_ptr));
    int value = co_await actor.template Send<&MoveOnlyArgsActor::GetLastValue>();
    EXPECT_EQ(value, 42);

    // unique_ptr<string>
    auto str = std::make_unique<std::string>("Hello");
    co_await actor.template Send<&MoveOnlyArgsActor::StoreString>(std::move(str));
    std::string stored = co_await actor.template Send<&MoveOnlyArgsActor::GetStoredString>();
    EXPECT_EQ(stored, "Hello");

    // unique_ptr<vector>
    auto vec_ptr = std::make_unique<std::vector<int>>(std::vector<int> {1, 2, 3, 4, 5});
    size_t size = co_await actor.template Send<&MoveOnlyArgsActor::ProcessVectorPtr>(std::move(vec_ptr));
    EXPECT_EQ(size, 5);
    bool has_vec = co_await actor.template Send<&MoveOnlyArgsActor::HasVectorPtr>();
    EXPECT_TRUE(has_vec);

    // Multiple unique_ptr arguments
    auto int_ptr2 = std::make_unique<int>(123);
    auto double_ptr = std::make_unique<double>(45.6);
    co_await actor.template Send<&MoveOnlyArgsActor::StoreMultiple>(std::move(int_ptr2), std::move(double_ptr));
    auto [i, d] = co_await actor.template Send<&MoveOnlyArgsActor::GetStoredValues>();
    EXPECT_EQ(i, 123);
    EXPECT_DOUBLE_EQ(d, 45.6);

    // unique_ptr<map>
    auto map_ptr =
        std::make_unique<std::map<std::string, int>>(std::map<std::string, int> {{"a", 1}, {"b", 2}, {"c", 3}});
    co_await actor.template Send<&MoveOnlyArgsActor::StoreMap>(std::move(map_ptr));
    size_t map_size = co_await actor.template Send<&MoveOnlyArgsActor::GetMapSize>();
    EXPECT_EQ(map_size, 3);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, ReferenceReturnTypes) {
  auto coroutine = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<ReferenceReturnActor>("Initial");
    std::string data = co_await actor.template Send<&ReferenceReturnActor::GetDataCopy>();
    EXPECT_EQ(data, "Initial");

    co_await actor.template Send<&ReferenceReturnActor::AppendData>(" Data");
    data = co_await actor.template Send<&ReferenceReturnActor::GetDataCopy>();
    EXPECT_EQ(data, "Initial Data");

    co_await actor.template Send<&ReferenceReturnActor::SetData>("New Data");
    data = co_await actor.template Send<&ReferenceReturnActor::GetDataCopy>();
    EXPECT_EQ(data, "New Data");

    size_t size = co_await actor.template Send<&ReferenceReturnActor::GetDataSize>();
    EXPECT_EQ(size, 8);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, ActorWithActorRefMember) {
  auto coroutine = []() -> exec::task<void> {
    auto child = co_await ex_actor::Spawn<ChildActor>();
    auto parent = co_await ex_actor::Spawn<ParentActor>(child);
    // Direct access to child
    co_await child.template Send<&ChildActor::IncrementCounter>();
    int count = co_await child.template Send<&ChildActor::GetCounter>();
    EXPECT_EQ(count, 1);

    // Access through parent
    co_await parent.template Send<&ParentActor::DelegateIncrement>();
    count = co_await parent.template Send<&ParentActor::GetChildCounter>();
    EXPECT_EQ(count, 2);

    co_await parent.template Send<&ParentActor::AddToChild>(10);
    count = co_await parent.template Send<&ParentActor::GetChildCounter>();
    EXPECT_EQ(count, 12);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, ActorWithMultipleActorRefMembers) {
  auto coroutine = []() -> exec::task<void> {
    auto acc1 = co_await ex_actor::Spawn<Accumulator>();
    auto acc2 = co_await ex_actor::Spawn<Accumulator>();
    auto acc3 = co_await ex_actor::Spawn<Accumulator>();

    auto distributor = co_await ex_actor::Spawn<Distributor>(acc1, acc2, acc3);
    // Distribute values
    co_await distributor.template Send<&Distributor::DistributeValue>(10);
    co_await distributor.template Send<&Distributor::DistributeValue>(20);
    co_await distributor.template Send<&Distributor::DistributeValue>(30);

    // Collect results
    auto sums = co_await distributor.template Send<&Distributor::CollectSums>();
    EXPECT_EQ(sums.size(), 3);

    // Each accumulator should have sum of 60 (10 + 20 + 30)
    for (int sum : sums) {
      EXPECT_EQ(sum, 60);
    }

    // Verify directly
    int sum1 = co_await acc1.template Send<&Accumulator::GetSum>();
    int sum2 = co_await acc2.template Send<&Accumulator::GetSum>();
    int sum3 = co_await acc3.template Send<&Accumulator::GetSum>();

    EXPECT_EQ(sum1, 60);
    EXPECT_EQ(sum2, 60);
    EXPECT_EQ(sum3, 60);
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, MixedComplexScenario) {
  auto coroutine = []() -> exec::task<void> {
    // Test combining multiple complex features
    // Create a complex hierarchy
    auto acc1 = co_await ex_actor::Spawn<Accumulator>();
    auto acc2 = co_await ex_actor::Spawn<Accumulator>();
    auto acc3 = co_await ex_actor::Spawn<Accumulator>();

    auto distributor = co_await ex_actor::Spawn<Distributor>(acc1, acc2, acc3);

    auto various_actor = co_await ex_actor::Spawn<VariousArgsActor>();
    auto move_actor = co_await ex_actor::Spawn<MoveOnlyArgsActor>();
    // Work with distributor
    co_await distributor.template Send<&Distributor::DistributeValue>(100);
    auto sums = co_await distributor.template Send<&Distributor::CollectSums>();
    EXPECT_EQ(sums.size(), 3);
    for (int sum : sums) {
      EXPECT_EQ(sum, 100);
    }

    // Work with various_actor
    co_await various_actor.template Send<&VariousArgsActor::SetState>(50);
    int result = co_await various_actor.template Send<&VariousArgsActor::Add>(10, 20);
    EXPECT_EQ(result, 30);

    // Work with move_actor
    auto str = std::make_unique<std::string>("Complex Test");
    co_await move_actor.template Send<&MoveOnlyArgsActor::StoreString>(std::move(str));
    std::string stored = co_await move_actor.template Send<&MoveOnlyArgsActor::GetStoredString>();
    EXPECT_EQ(stored, "Complex Test");
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
