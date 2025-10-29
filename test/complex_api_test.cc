#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

// ===========================
// Actor 1: No Constructor Parameters
// ===========================

class EmptyActor {
 public:
  EmptyActor() = default;

  int GetCounter() const { return counter_; }

  void Increment() { counter_++; }

  void Add(int value) { counter_ += value; }

 private:
  int counter_ = 0;
};

// ===========================
// Actor 2: Single Parameter Constructor (Copiable)
// ===========================

class SingleParamActor {
 public:
  explicit SingleParamActor(std::string name) : name_(std::move(name)) {}

  std::string GetName() const { return name_; }

  void SetValue(int value) { value_ = value; }

  int GetValue() const { return value_; }

  void AppendToName(const std::string& suffix) { name_ += suffix; }

 private:
  std::string name_;
  int value_ = 0;
};

// ===========================
// Actor 3: Multiple Parameters Constructor (Mixed Types)
// ===========================

class MultiParamActor {
 public:
  MultiParamActor(int id, std::string name, double scale, bool enabled)
      : id_(id), name_(std::move(name)), scale_(scale), enabled_(enabled) {}

  int GetId() const { return id_; }
  std::string GetName() const { return name_; }
  double GetScale() const { return scale_; }
  bool IsEnabled() const { return enabled_; }

  void UpdateState(int new_id, std::string new_name, double new_scale, bool new_enabled) {
    id_ = new_id;
    name_ = std::move(new_name);
    scale_ = new_scale;
    enabled_ = new_enabled;
  }

  std::string GetDescription() const {
    return "Actor{id=" + std::to_string(id_) + ", name=" + name_ + ", scale=" + std::to_string(scale_) +
           ", enabled=" + (enabled_ ? "true" : "false") + "}";
  }

 private:
  int id_;
  std::string name_;
  double scale_;
  bool enabled_;
};

// ===========================
// Actor 4: Move-only Constructor Parameters
// ===========================

class MoveOnlyConstructorActor {
 public:
  explicit MoveOnlyConstructorActor(std::unique_ptr<std::string> data) : data_(std::move(data)) {}

  std::string GetData() const { return data_ ? *data_ : "null"; }

  void SetData(std::unique_ptr<std::string>&& new_data, const std::string&) { data_ = std::move(new_data); }

  size_t GetDataLength() const { return data_ ? data_->length() : 0; }

 private:
  std::unique_ptr<std::string> data_;
};

// ===========================
// Actor 5: Complex Constructor with Containers
// ===========================

class ComplexContainerActor {
 public:
  ComplexContainerActor(std::vector<int> ids, std::map<std::string, int> lookup, std::optional<std::string> description)
      : ids_(std::move(ids)), lookup_(std::move(lookup)), description_(std::move(description)) {}

  std::vector<int> GetIds() const { return ids_; }

  size_t GetIdsSize() const { return ids_.size(); }

  int GetIdAt(size_t index) const { return index < ids_.size() ? ids_[index] : -1; }

  std::optional<int> Lookup(const std::string& key) const {
    auto it = lookup_.find(key);
    if (it != lookup_.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  std::string GetDescription() const { return description_.value_or("No description"); }

  void AddEntry(std::string key, int value) { lookup_[std::move(key)] = value; }

  void AppendId(int id) { ids_.push_back(id); }

 private:
  std::vector<int> ids_;
  std::map<std::string, int> lookup_;
  std::optional<std::string> description_;
};

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

TEST(ComplexApiTest, EmptyConstructorActor) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<EmptyActor>();

  auto workflow = [&]() -> exec::task<void> {
    int initial = co_await actor.template Send<&EmptyActor::GetCounter>();
    EXPECT_EQ(initial, 0);

    co_await actor.template Send<&EmptyActor::Increment>();
    co_await actor.template Send<&EmptyActor::Increment>();

    int after_increment = co_await actor.template Send<&EmptyActor::GetCounter>();
    EXPECT_EQ(after_increment, 2);

    co_await actor.template Send<&EmptyActor::Add>(5);

    int final_count = co_await actor.template Send<&EmptyActor::GetCounter>();
    EXPECT_EQ(final_count, 7);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, SingleParameterConstructor) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<SingleParamActor>("TestActor");

  auto workflow = [&]() -> exec::task<void> {
    std::string name = co_await actor.template Send<&SingleParamActor::GetName>();
    EXPECT_EQ(name, "TestActor");

    co_await actor.template Send<&SingleParamActor::SetValue>(42);
    int value = co_await actor.template Send<&SingleParamActor::GetValue>();
    EXPECT_EQ(value, 42);

    co_await actor.template Send<&SingleParamActor::AppendToName>("_Suffix");
    std::string new_name = co_await actor.template Send<&SingleParamActor::GetName>();
    EXPECT_EQ(new_name, "TestActor_Suffix");
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, MultipleParametersConstructor) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<MultiParamActor>(123, "MultiActor", 2.5, true);

  auto workflow = [&]() -> exec::task<void> {
    int id = co_await actor.template Send<&MultiParamActor::GetId>();
    EXPECT_EQ(id, 123);

    std::string name = co_await actor.template Send<&MultiParamActor::GetName>();
    EXPECT_EQ(name, "MultiActor");

    double scale = co_await actor.template Send<&MultiParamActor::GetScale>();
    EXPECT_DOUBLE_EQ(scale, 2.5);

    bool enabled = co_await actor.template Send<&MultiParamActor::IsEnabled>();
    EXPECT_TRUE(enabled);

    std::string desc = co_await actor.template Send<&MultiParamActor::GetDescription>();
    EXPECT_FALSE(desc.empty());

    co_await actor.template Send<&MultiParamActor::UpdateState>(999, "Updated", 3.14, false);

    int new_id = co_await actor.template Send<&MultiParamActor::GetId>();
    EXPECT_EQ(new_id, 999);

    bool new_enabled = co_await actor.template Send<&MultiParamActor::IsEnabled>();
    EXPECT_FALSE(new_enabled);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, MoveOnlyConstructorParameters) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto initial_data = std::make_unique<std::string>("Initial");
  auto actor = registry.CreateActor<MoveOnlyConstructorActor>(std::move(initial_data));

  auto workflow = [&]() -> exec::task<void> {
    std::string data = co_await actor.template Send<&MoveOnlyConstructorActor::GetData>();
    EXPECT_EQ(data, "Initial");

    auto new_data = std::make_unique<std::string>("Updated");
    std::string lvalue;
    co_await actor.template Send<&MoveOnlyConstructorActor::SetData>(std::move(new_data), lvalue);

    std::string updated_data = co_await actor.template Send<&MoveOnlyConstructorActor::GetData>();
    EXPECT_EQ(updated_data, "Updated");
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, ComplexContainerConstructor) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  std::vector<int> ids = {1, 2, 3, 4, 5};
  std::map<std::string, int> lookup = {{"a", 10}, {"b", 20}, {"c", 30}};
  std::optional<std::string> description = "Test Actor";

  auto actor = registry.CreateActor<ComplexContainerActor>(std::move(ids), std::move(lookup), std::move(description));

  auto workflow = [&]() -> exec::task<void> {
    auto retrieved_ids = co_await actor.template Send<&ComplexContainerActor::GetIds>();
    EXPECT_EQ(retrieved_ids.size(), 5);
    EXPECT_EQ(retrieved_ids[0], 1);
    EXPECT_EQ(retrieved_ids[4], 5);

    size_t ids_size = co_await actor.template Send<&ComplexContainerActor::GetIdsSize>();
    EXPECT_EQ(ids_size, 5);

    int first_id = co_await actor.template Send<&ComplexContainerActor::GetIdAt>(0);
    EXPECT_EQ(first_id, 1);

    auto value_a = co_await actor.template Send<&ComplexContainerActor::Lookup>("a");
    EXPECT_TRUE(value_a.has_value());
    EXPECT_EQ(value_a.value(), 10);

    auto value_missing = co_await actor.template Send<&ComplexContainerActor::Lookup>("missing");
    EXPECT_FALSE(value_missing.has_value());

    std::string desc = co_await actor.template Send<&ComplexContainerActor::GetDescription>();
    EXPECT_EQ(desc, "Test Actor");

    co_await actor.template Send<&ComplexContainerActor::AddEntry>("d", 40);
    auto value_d = co_await actor.template Send<&ComplexContainerActor::Lookup>("d");
    EXPECT_TRUE(value_d.has_value());
    EXPECT_EQ(value_d.value(), 40);

    co_await actor.template Send<&ComplexContainerActor::AppendId>(6);
    size_t new_size = co_await actor.template Send<&ComplexContainerActor::GetIdsSize>();
    EXPECT_EQ(new_size, 6);
    int last_id = co_await actor.template Send<&ComplexContainerActor::GetIdAt>(5);
    EXPECT_EQ(last_id, 6);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, VariousArgumentTypes) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<VariousArgsActor>();

  auto workflow = [&]() -> exec::task<void> {
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

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, MoveOnlyArguments) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<MoveOnlyArgsActor>();

  auto workflow = [&]() -> exec::task<void> {
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

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, ReferenceReturnTypes) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<ReferenceReturnActor>("Initial");

  auto workflow = [&]() -> exec::task<void> {
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

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, ActorWithActorRefMember) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto child = registry.CreateActor<ChildActor>();
  auto parent = registry.CreateActor<ParentActor>(child);

  auto workflow = [&]() -> exec::task<void> {
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

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, ActorWithMultipleActorRefMembers) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto acc1 = registry.CreateActor<Accumulator>();
  auto acc2 = registry.CreateActor<Accumulator>();
  auto acc3 = registry.CreateActor<Accumulator>();

  auto distributor = registry.CreateActor<Distributor>(acc1, acc2, acc3);

  auto workflow = [&]() -> exec::task<void> {
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

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, MixedComplexScenario) {
  // Test combining multiple complex features
  ex_actor::WorkSharingThreadPool thread_pool(8);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  // Create a complex hierarchy
  auto acc1 = registry.CreateActor<Accumulator>();
  auto acc2 = registry.CreateActor<Accumulator>();
  auto acc3 = registry.CreateActor<Accumulator>();

  auto distributor = registry.CreateActor<Distributor>(acc1, acc2, acc3);

  auto multi_actor = registry.CreateActor<MultiParamActor>(1, "Complex", 1.5, true);
  auto various_actor = registry.CreateActor<VariousArgsActor>();
  auto move_actor = registry.CreateActor<MoveOnlyArgsActor>();

  auto workflow = [&]() -> exec::task<void> {
    // Work with distributor
    co_await distributor.template Send<&Distributor::DistributeValue>(100);
    auto sums = co_await distributor.template Send<&Distributor::CollectSums>();
    EXPECT_EQ(sums.size(), 3);
    for (int sum : sums) {
      EXPECT_EQ(sum, 100);
    }

    // Work with multi_actor
    co_await multi_actor.template Send<&MultiParamActor::UpdateState>(999, "Updated", 2.0, false);
    int id = co_await multi_actor.template Send<&MultiParamActor::GetId>();
    EXPECT_EQ(id, 999);

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

  ex::sync_wait(workflow());
}

// ===========================
// Custom Structs for Testing
// ===========================

struct Point {
  double x;
  double y;

  double Distance() const { return std::sqrt((x * x) + (y * y)); }
};

struct Person {
  std::string name;
  int age = 0;
  std::string email;

  std::string Summary() const { return name + " (" + std::to_string(age) + " years old)"; }
};

struct DataPacket {
  int id = 0;
  std::vector<double> values;
  std::map<std::string, int> metadata;

  size_t TotalSize() const { return values.size() + metadata.size(); }
};

struct Configuration {
  std::string app_name;
  int version = 0;
  std::map<std::string, std::string> settings;
  std::vector<std::string> features;
  bool is_production = false;

  std::string ToString() const { return app_name + " v" + std::to_string(version); }
};

struct MoveOnlyData {
  std::unique_ptr<std::string> content;
  std::unique_ptr<std::vector<int>> numbers;
};

// ===========================
// Actor 11: Actor with Struct Arguments
// ===========================

class StructActor {
 public:
  StructActor() = default;

  void SetPoint(Point p) { point_ = p; }

  Point GetPoint() const { return point_; }

  double GetPointDistance() const { return point_.Distance(); }

  void ProcessPerson(Person person) { person_ = std::move(person); }

  Person GetPerson() const { return person_; }

  std::string GetPersonSummary() const { return person_.Summary(); }

  void UpdateDataPacket(DataPacket packet) { packet_ = std::move(packet); }

  DataPacket GetDataPacket() const { return packet_; }

  size_t GetPacketSize() const { return packet_.TotalSize(); }

  double SumPacketValues() const {
    double sum = 0.0;
    for (double v : packet_.values) {
      sum += v;
    }
    return sum;
  }

 private:
  Point point_ {.x = 0.0, .y = 0.0};
  Person person_ {.name = "", .age = 0, .email = ""};
  DataPacket packet_ {.id = 0, .values = {}, .metadata = {}};
};

// ===========================
// Actor 12: Actor with Struct in Constructor
// ===========================

class StructConstructorActor {
 public:
  explicit StructConstructorActor(Configuration config) : config_(std::move(config)) {}

  Configuration GetConfig() const { return config_; }

  std::string GetConfigString() const { return config_.ToString(); }

  void UpdateConfig(Configuration new_config) { config_ = std::move(new_config); }

  bool IsProduction() const { return config_.is_production; }

  size_t GetFeatureCount() const { return config_.features.size(); }

  std::optional<std::string> GetSetting(const std::string& key) const {
    auto it = config_.settings.find(key);
    if (it != config_.settings.end()) {
      return it->second;
    }
    return std::nullopt;
  }

 private:
  Configuration config_;
};

// ===========================
// Actor 13: Actor with Multiple Struct Arguments
// ===========================

class MultiStructActor {
 public:
  MultiStructActor() = default;

  void ProcessPointAndPerson(Point pt, Person person) {
    point_ = pt;
    person_ = std::move(person);
  }

  std::string GetCombinedInfo() const {
    return person_.name + " at (" + std::to_string(point_.x) + ", " + std::to_string(point_.y) + ")";
  }

  void SetMultipleStructs(Point pt, DataPacket packet, Configuration config) {
    point_ = pt;
    packet_ = std::move(packet);
    config_ = std::move(config);
  }

  std::string GetFullSummary() const {
    return "Point: (" + std::to_string(point_.x) + ", " + std::to_string(point_.y) +
           "), Packet ID: " + std::to_string(packet_.id) + ", Config: " + config_.ToString();
  }

  bool HasData() const { return packet_.id > 0; }

 private:
  Point point_ {.x = 0.0, .y = 0.0};
  Person person_ {.name = "", .age = 0, .email = ""};
  DataPacket packet_ {.id = 0, .values = {}, .metadata = {}};
  Configuration config_ {.app_name = "", .version = 0, .settings = {}, .features = {}, .is_production = false};
};

// ===========================
// Actor 14: Actor with Move-only Struct
// ===========================

class MoveOnlyStructActor {
 public:
  MoveOnlyStructActor() = default;

  void StoreMoveOnlyData(MoveOnlyData data) { data_ = std::make_unique<MoveOnlyData>(std::move(data)); }

  std::string GetContent() const {
    if (data_ && data_->content) {
      return *data_->content;
    }
    return "empty";
  }

  size_t GetNumbersSize() const {
    if (data_ && data_->numbers) {
      return data_->numbers->size();
    }
    return 0;
  }

  int GetNumberAt(size_t index) const {
    if (data_ && data_->numbers && index < data_->numbers->size()) {
      return (*data_->numbers)[index];
    }
    return -1;
  }

  bool HasData() const { return data_ != nullptr; }

 private:
  std::unique_ptr<MoveOnlyData> data_;
};

// ===========================
// Actor 15: Actor with Nested Structs
// ===========================

struct Address {
  std::string street;
  std::string city;
  std::string country;
  int zip_code = 0;

  std::string FullAddress() const { return street + ", " + city + ", " + country + " " + std::to_string(zip_code); }
};

struct Employee {
  Person person_info;
  Address address;
  int employee_id = 0;
  double salary = 0.0;

  std::string GetInfo() const { return person_info.Summary() + " - ID: " + std::to_string(employee_id); }
};

class NestedStructActor {
 public:
  NestedStructActor() = default;

  void SetEmployee(Employee emp) { employee_ = std::move(emp); }

  Employee GetEmployee() const { return employee_; }

  std::string GetEmployeeInfo() const { return employee_.GetInfo(); }

  std::string GetFullAddress() const { return employee_.address.FullAddress(); }

  double GetSalary() const { return employee_.salary; }

  int GetAge() const { return employee_.person_info.age; }

 private:
  Employee employee_ {.person_info = {}, .address = {}, .employee_id = 0, .salary = 0.0};
};

// ===========================
// Test Cases for Custom Structs
// ===========================

TEST(ComplexApiTest, StructArguments) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<StructActor>();

  auto workflow = [&]() -> exec::task<void> {
    // Test Point struct
    Point pt {.x = 3.0, .y = 4.0};
    co_await actor.template Send<&StructActor::SetPoint>(pt);

    Point retrieved = co_await actor.template Send<&StructActor::GetPoint>();
    EXPECT_DOUBLE_EQ(retrieved.x, 3.0);
    EXPECT_DOUBLE_EQ(retrieved.y, 4.0);

    double distance = co_await actor.template Send<&StructActor::GetPointDistance>();
    EXPECT_DOUBLE_EQ(distance, 5.0);

    // Test Person struct
    Person person {.name = "Alice", .age = 30, .email = "alice@example.com"};
    co_await actor.template Send<&StructActor::ProcessPerson>(std::move(person));

    Person retrieved_person = co_await actor.template Send<&StructActor::GetPerson>();
    EXPECT_EQ(retrieved_person.name, "Alice");
    EXPECT_EQ(retrieved_person.age, 30);
    EXPECT_EQ(retrieved_person.email, "alice@example.com");

    std::string summary = co_await actor.template Send<&StructActor::GetPersonSummary>();
    EXPECT_EQ(summary, "Alice (30 years old)");

    // Test DataPacket struct
    DataPacket packet;
    packet.id = 42;
    packet.values = {1.1, 2.2, 3.3, 4.4};
    packet.metadata = {{"type", 1}, {"priority", 5}};

    co_await actor.template Send<&StructActor::UpdateDataPacket>(std::move(packet));

    DataPacket retrieved_packet = co_await actor.template Send<&StructActor::GetDataPacket>();
    EXPECT_EQ(retrieved_packet.id, 42);
    EXPECT_EQ(retrieved_packet.values.size(), 4);

    size_t packet_size = co_await actor.template Send<&StructActor::GetPacketSize>();
    EXPECT_EQ(packet_size, 6);  // 4 values + 2 metadata

    double sum = co_await actor.template Send<&StructActor::SumPacketValues>();
    EXPECT_DOUBLE_EQ(sum, 11.0);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, StructInConstructor) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  Configuration config;
  config.app_name = "TestApp";
  config.version = 2;
  config.settings = {{"timeout", "30"}, {"retry", "3"}};
  config.features = {"feature1", "feature2", "feature3"};
  config.is_production = true;

  auto actor = registry.CreateActor<StructConstructorActor>(std::move(config));

  auto workflow = [&]() -> exec::task<void> {
    Configuration retrieved = co_await actor.template Send<&StructConstructorActor::GetConfig>();
    EXPECT_EQ(retrieved.app_name, "TestApp");
    EXPECT_EQ(retrieved.version, 2);
    EXPECT_TRUE(retrieved.is_production);

    std::string config_str = co_await actor.template Send<&StructConstructorActor::GetConfigString>();
    EXPECT_EQ(config_str, "TestApp v2");

    bool is_prod = co_await actor.template Send<&StructConstructorActor::IsProduction>();
    EXPECT_TRUE(is_prod);

    size_t feature_count = co_await actor.template Send<&StructConstructorActor::GetFeatureCount>();
    EXPECT_EQ(feature_count, 3);

    auto timeout = co_await actor.template Send<&StructConstructorActor::GetSetting>("timeout");
    EXPECT_TRUE(timeout.has_value());
    EXPECT_EQ(timeout.value(), "30");

    auto missing = co_await actor.template Send<&StructConstructorActor::GetSetting>("missing");
    EXPECT_FALSE(missing.has_value());

    // Update config
    Configuration new_config;
    new_config.app_name = "UpdatedApp";
    new_config.version = 3;
    new_config.settings = {{"max_connections", "100"}};
    new_config.features = {"new_feature"};
    new_config.is_production = false;

    co_await actor.template Send<&StructConstructorActor::UpdateConfig>(std::move(new_config));

    std::string updated_str = co_await actor.template Send<&StructConstructorActor::GetConfigString>();
    EXPECT_EQ(updated_str, "UpdatedApp v3");

    bool is_prod_after = co_await actor.template Send<&StructConstructorActor::IsProduction>();
    EXPECT_FALSE(is_prod_after);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, MultipleStructArguments) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<MultiStructActor>();

  auto workflow = [&]() -> exec::task<void> {
    // Test with Point and Person
    Point pt {.x = 10.0, .y = 20.0};
    Person person {.name = "Bob", .age = 25, .email = "bob@example.com"};

    co_await actor.template Send<&MultiStructActor::ProcessPointAndPerson>(pt, std::move(person));

    std::string info = co_await actor.template Send<&MultiStructActor::GetCombinedInfo>();
    EXPECT_NE(info.find("Bob"), std::string::npos);
    EXPECT_NE(info.find("10."), std::string::npos);
    EXPECT_NE(info.find("20."), std::string::npos);

    // Test with three structs
    Point pt2 {.x = 5.0, .y = 15.0};
    DataPacket packet;
    packet.id = 123;
    packet.values = {1.0, 2.0, 3.0};

    Configuration config;
    config.app_name = "MultiStruct";
    config.version = 1;
    config.is_production = false;

    co_await actor.template Send<&MultiStructActor::SetMultipleStructs>(pt2, std::move(packet), std::move(config));

    std::string summary = co_await actor.template Send<&MultiStructActor::GetFullSummary>();
    EXPECT_NE(summary.find("5."), std::string::npos);
    EXPECT_NE(summary.find("123"), std::string::npos);
    EXPECT_NE(summary.find("MultiStruct"), std::string::npos);

    bool has_data = co_await actor.template Send<&MultiStructActor::HasData>();
    EXPECT_TRUE(has_data);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, MoveOnlyStructArgument) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<MoveOnlyStructActor>();

  auto workflow = [&]() -> exec::task<void> {
    // Create move-only data
    auto content = std::make_unique<std::string>("Test Content");
    auto numbers = std::make_unique<std::vector<int>>(std::vector<int> {10, 20, 30, 40, 50});

    MoveOnlyData data(std::move(content), std::move(numbers));

    co_await actor.template Send<&MoveOnlyStructActor::StoreMoveOnlyData>(std::move(data));

    std::string retrieved_content = co_await actor.template Send<&MoveOnlyStructActor::GetContent>();
    EXPECT_EQ(retrieved_content, "Test Content");

    size_t size = co_await actor.template Send<&MoveOnlyStructActor::GetNumbersSize>();
    EXPECT_EQ(size, 5);

    int first = co_await actor.template Send<&MoveOnlyStructActor::GetNumberAt>(0);
    EXPECT_EQ(first, 10);

    int last = co_await actor.template Send<&MoveOnlyStructActor::GetNumberAt>(4);
    EXPECT_EQ(last, 50);

    bool has_data = co_await actor.template Send<&MoveOnlyStructActor::HasData>();
    EXPECT_TRUE(has_data);
  };

  ex::sync_wait(workflow());
}

TEST(ComplexApiTest, NestedStructArgument) {
  ex_actor::WorkSharingThreadPool thread_pool(4);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  auto actor = registry.CreateActor<NestedStructActor>();

  auto workflow = [&]() -> exec::task<void> {
    // Create nested struct
    Employee emp;
    emp.person_info = {.name = "Charlie", .age = 35, .email = "charlie@example.com"};
    emp.address = {.street = "123 Main St", .city = "New York", .country = "USA", .zip_code = 10001};
    emp.employee_id = 1001;
    emp.salary = 75000.0;

    co_await actor.template Send<&NestedStructActor::SetEmployee>(std::move(emp));

    Employee retrieved = co_await actor.template Send<&NestedStructActor::GetEmployee>();
    EXPECT_EQ(retrieved.person_info.name, "Charlie");
    EXPECT_EQ(retrieved.person_info.age, 35);
    EXPECT_EQ(retrieved.employee_id, 1001);
    EXPECT_DOUBLE_EQ(retrieved.salary, 75000.0);
    EXPECT_EQ(retrieved.address.city, "New York");
    EXPECT_EQ(retrieved.address.zip_code, 10001);

    std::string info = co_await actor.template Send<&NestedStructActor::GetEmployeeInfo>();
    EXPECT_NE(info.find("Charlie"), std::string::npos);
    EXPECT_NE(info.find("1001"), std::string::npos);

    std::string address = co_await actor.template Send<&NestedStructActor::GetFullAddress>();
    EXPECT_NE(address.find("Main St"), std::string::npos);
    EXPECT_NE(address.find("New York"), std::string::npos);
    EXPECT_NE(address.find("10001"), std::string::npos);

    double salary = co_await actor.template Send<&NestedStructActor::GetSalary>();
    EXPECT_DOUBLE_EQ(salary, 75000.0);

    int age = co_await actor.template Send<&NestedStructActor::GetAge>();
    EXPECT_EQ(age, 35);
  };

  ex::sync_wait(workflow());
}
