#include <map>
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
// Test Cases
// ===========================

TEST(ComplexApiTest, EmptyConstructorActor) {
  auto coroutine = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<EmptyActor>();

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
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, SingleParameterConstructor) {
  auto coroutine = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<SingleParamActor>("TestActor");

    std::string name = co_await actor.template Send<&SingleParamActor::GetName>();
    EXPECT_EQ(name, "TestActor");

    co_await actor.template Send<&SingleParamActor::SetValue>(42);
    int value = co_await actor.template Send<&SingleParamActor::GetValue>();
    EXPECT_EQ(value, 42);

    co_await actor.template Send<&SingleParamActor::AppendToName>("_Suffix");
    std::string new_name = co_await actor.template Send<&SingleParamActor::GetName>();
    EXPECT_EQ(new_name, "TestActor_Suffix");
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, MultipleParametersConstructor) {
  auto coroutine = []() -> exec::task<void> {
    auto actor = co_await ex_actor::Spawn<MultiParamActor>(123, "MultiActor", 2.5, true);

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
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, MoveOnlyConstructorParameters) {
  auto coroutine = []() -> exec::task<void> {
    auto initial_data = std::make_unique<std::string>("Initial");
    auto actor = co_await ex_actor::Spawn<MoveOnlyConstructorActor>(std::move(initial_data));

    std::string data = co_await actor.template Send<&MoveOnlyConstructorActor::GetData>();
    EXPECT_EQ(data, "Initial");

    auto new_data = std::make_unique<std::string>("Updated");
    std::string lvalue;
    co_await actor.template Send<&MoveOnlyConstructorActor::SetData>(std::move(new_data), lvalue);

    std::string updated_data = co_await actor.template Send<&MoveOnlyConstructorActor::GetData>();
    EXPECT_EQ(updated_data, "Updated");
  };
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}

TEST(ComplexApiTest, ComplexContainerConstructor) {
  auto coroutine = []() -> exec::task<void> {
    std::vector<int> ids = {1, 2, 3, 4, 5};
    std::map<std::string, int> lookup = {{"a", 10}, {"b", 20}, {"c", 30}};
    std::optional<std::string> description = "Test Actor";

    auto actor =
        co_await ex_actor::Spawn<ComplexContainerActor>(std::move(ids), std::move(lookup), std::move(description));
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
  ex_actor::Init(/*thread_pool_size=*/10);
  ex::sync_wait(coroutine());
  ex_actor::Shutdown();
}
