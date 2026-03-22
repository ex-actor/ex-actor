#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace ex = stdexec;

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
  auto coroutine = []() -> exec::task<void> {
    co_await ex_actor::Start(/*thread_pool_size=*/10);
    auto actor = co_await ex_actor::Spawn<StructActor>();
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
    co_await ex_actor::Stop();
  };
  ex::sync_wait(coroutine());
}

TEST(ComplexApiTest, StructInConstructor) {
  auto coroutine = []() -> exec::task<void> {
    co_await ex_actor::Start(/*thread_pool_size=*/10);
    Configuration config;
    config.app_name = "TestApp";
    config.version = 2;
    config.settings = {{"timeout", "30"}, {"retry", "3"}};
    config.features = {"feature1", "feature2", "feature3"};
    config.is_production = true;

    auto actor = co_await ex_actor::Spawn<StructConstructorActor>(std::move(config));
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
    co_await ex_actor::Stop();
  };
  ex::sync_wait(coroutine());
}

TEST(ComplexApiTest, MultipleStructArguments) {
  auto coroutine = []() -> exec::task<void> {
    co_await ex_actor::Start(/*thread_pool_size=*/10);
    auto actor = co_await ex_actor::Spawn<MultiStructActor>();
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
    co_await ex_actor::Stop();
  };
  ex::sync_wait(coroutine());
}

TEST(ComplexApiTest, MoveOnlyStructArgument) {
  auto coroutine = []() -> exec::task<void> {
    co_await ex_actor::Start(/*thread_pool_size=*/10);
    auto actor = co_await ex_actor::Spawn<MoveOnlyStructActor>();
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
    co_await ex_actor::Stop();
  };
  ex::sync_wait(coroutine());
}

TEST(ComplexApiTest, NestedStructArgument) {
  auto coroutine = []() -> exec::task<void> {
    co_await ex_actor::Start(/*thread_pool_size=*/10);
    auto actor = co_await ex_actor::Spawn<NestedStructActor>();
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
    co_await ex_actor::Stop();
  };
  ex::sync_wait(coroutine());
}
