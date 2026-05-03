#include "ex_actor/internal/logging.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ex_actor/api.h"

namespace fs = std::filesystem;

namespace {

// Helper function to read file contents
std::string ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// Helper function to check if string contains substring
bool Contains(const std::string& str, const std::string& substring) { return str.find(substring) != std::string::npos; }

// Helper function to clean up log file (resets logger to close file handle on Windows)
void CleanupLogFile(const std::string& log_file) {
  if (fs::exists(log_file)) {
    // Reset logger to close file handle before cleanup (required on Windows)
    ex_actor::ConfigureLogging({});
    fs::remove(log_file);
  }
}

}  // namespace

// Test 1: Init() and Shutdown() without configure logging, should see all logs
TEST(LoggingTest, InitShutdownWithoutConfigureLogging) {
  std::string log_file = "test_log_1.txt";

  // Clean up any existing log file
  CleanupLogFile(log_file);

  // Configure logging to file with default Info level
  ex_actor::ConfigureLogging({
      .log_file_path = log_file,
  });

  // Init and Shutdown
  ex_actor::Init(4);
  ex_actor::Shutdown();

  // flush log
  ex_actor::internal::GlobalLogger()->flush();

  // Read log file and verify both Init and Shutdown logs are present
  std::string log_contents = ReadFile(log_file);
  EXPECT_FALSE(log_contents.empty()) << "Log file should not be empty";
  EXPECT_TRUE(Contains(log_contents, "Initializing ex_actor")) << "Should see Init log. Log contents:\n"
                                                               << log_contents;
  EXPECT_TRUE(Contains(log_contents, "Shutting down ex_actor")) << "Should see Shutdown log. Log contents:\n"
                                                                << log_contents;

  // Clean up
  CleanupLogFile(log_file);
}

// Test 2: ConfigureLogging() with error level, should see no info log
TEST(LoggingTest, ConfigureLoggingWithErrorLevel) {
  std::string log_file = "test_log_2.txt";

  // Clean up any existing log file
  CleanupLogFile(log_file);

  // Configure logging to file with Error level (should filter out Info logs)
  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kError,
      .log_file_path = log_file,
  });

  // Init and Shutdown - these produce Info level logs
  ex_actor::Init(4);
  ex_actor::Shutdown();

  // flush log
  ex_actor::internal::GlobalLogger()->flush();

  // Read log file - should be empty or not contain Info logs
  std::string log_contents = ReadFile(log_file);
  EXPECT_FALSE(Contains(log_contents, "Initializing ex_actor"))
      << "Should NOT see Init log at Error level. Log contents:\n"
      << log_contents;
  EXPECT_FALSE(Contains(log_contents, "Shutting down ex_actor"))
      << "Should NOT see Shutdown log at Error level. Log contents:\n"
      << log_contents;

  // Clean up
  CleanupLogFile(log_file);
}

// Test 3: Init() first with Info level, then ConfigureLogging() with Error level in the middle,
// and Shutdown(). Should only see Init log, not Shutdown log
TEST(LoggingTest, ConfigureLoggingInMiddle) {
  std::string log_file = "test_log_3.txt";

  // Clean up any existing log file
  CleanupLogFile(log_file);

  // Configure logging to file with Info level initially
  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kInfo,
      .log_file_path = log_file,
  });

  // Init - should be logged
  ex_actor::Init(4);

  // Change log level to Error in the middle
  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kError,
      .log_file_path = log_file,
  });

  // Shutdown - should NOT be logged because level is now Error
  ex_actor::Shutdown();

  // flush log
  ex_actor::internal::GlobalLogger()->flush();

  // Read log file
  std::string log_contents = ReadFile(log_file);
  EXPECT_FALSE(log_contents.empty()) << "Log file should not be empty";
  EXPECT_TRUE(Contains(log_contents, "Initializing ex_actor"))
      << "Should see Init log (before level change). Log contents:\n"
      << log_contents;
  EXPECT_FALSE(Contains(log_contents, "Shutting down ex_actor"))
      << "Should NOT see Shutdown log (after level change to Error). Log contents:\n"
      << log_contents;

  // Clean up
  CleanupLogFile(log_file);
}

// Test 4: ConfigureLogging() should be thread-safe with concurrent logging.
TEST(LoggingTest, ConfigureLoggingConcurrentWithSharedLoggerUsage) {
  const std::string log_file_1 = "test_log_4_1.txt";
  const std::string log_file_2 = "test_log_4_2.txt";

  CleanupLogFile(log_file_1);
  CleanupLogFile(log_file_2);

  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kInfo,
      .log_file_path = log_file_1,
  });

  std::atomic<bool> stop = false;
  std::atomic<int> write_count = 0;

  std::thread writer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      auto logger = ex_actor::internal::GlobalLogger();
      std::this_thread::yield();
      logger->info("concurrent log {}", write_count.fetch_add(1, std::memory_order_relaxed));
    }
  });

  for (int i = 0; i < 200000; ++i) {
    ex_actor::ConfigureLogging({
        .level = ex_actor::LogLevel::kInfo,
        .log_file_path = (i % 2 == 0) ? log_file_1 : log_file_2,
    });
  }

  stop.store(true, std::memory_order_relaxed);
  writer.join();

  ex_actor::internal::GlobalLogger()->flush();

  // A thread-safe implementation should preserve writer progress.
  EXPECT_GT(write_count.load(std::memory_order_relaxed), 0);

  CleanupLogFile(log_file_1);
  CleanupLogFile(log_file_2);
}

// Test 5: Stronger stress test with multiple writers and multiple reconfiguration threads.
TEST(LoggingTest, ConfigureLoggingHighContentionStress) {
  const std::string log_file_1 = "test_log_5_1.txt";
  const std::string log_file_2 = "test_log_5_2.txt";
  const std::string log_file_3 = "test_log_5_3.txt";

  CleanupLogFile(log_file_1);
  CleanupLogFile(log_file_2);
  CleanupLogFile(log_file_3);

  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kInfo,
      .log_file_path = log_file_1,
  });

  constexpr int kWriterThreadCount = 6;
  constexpr int kWriterIterationsPerThread = 100000;
  constexpr int kConfigThreadCount = 3;
  constexpr int kConfigIterationsPerThread = 80000;

  std::atomic<bool> start = false;
  std::atomic<int> total_writes = 0;
  std::atomic<int> total_reconfigurations = 0;

  std::vector<std::thread> threads;
  threads.reserve(kWriterThreadCount + kConfigThreadCount);

  for (int writer_id = 0; writer_id < kWriterThreadCount; ++writer_id) {
    threads.emplace_back([&, writer_id]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int i = 0; i < kWriterIterationsPerThread; ++i) {
        auto logger = ex_actor::internal::GlobalLogger();
        logger->info("stress writer={} seq={}", writer_id, i);
        total_writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int config_id = 0; config_id < kConfigThreadCount; ++config_id) {
    threads.emplace_back([&, config_id]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int i = 0; i < kConfigIterationsPerThread; ++i) {
        const int seq = total_reconfigurations.fetch_add(1, std::memory_order_relaxed);
        const std::string* target_file = &log_file_1;
        if ((seq % 3) == 1) {
          target_file = &log_file_2;
        } else if ((seq % 3) == 2) {
          target_file = &log_file_3;
        }
        ex_actor::ConfigureLogging({
            .level = ex_actor::LogLevel::kInfo,
            .log_file_path = *target_file,
        });
        if ((i % 4096) == 0) {
          ex_actor::internal::GlobalLogger()->flush();
        }
      }

      if (config_id == 0) {
        ex_actor::ConfigureLogging({
            .level = ex_actor::LogLevel::kInfo,
            .log_file_path = log_file_1,
        });
      }
    });
  }

  start.store(true, std::memory_order_release);

  for (auto& thread : threads) {
    thread.join();
  }

  ex_actor::internal::GlobalLogger()->flush();

  EXPECT_EQ(total_writes.load(std::memory_order_relaxed), kWriterThreadCount * kWriterIterationsPerThread);
  EXPECT_EQ(total_reconfigurations.load(std::memory_order_relaxed), kConfigThreadCount * kConfigIterationsPerThread);

  const std::string log_1_contents = ReadFile(log_file_1);
  const std::string log_2_contents = ReadFile(log_file_2);
  const std::string log_3_contents = ReadFile(log_file_3);

  EXPECT_TRUE(!log_1_contents.empty() || !log_2_contents.empty() || !log_3_contents.empty())
      << "At least one stress log file should contain log lines.";

  CleanupLogFile(log_file_1);
  CleanupLogFile(log_file_2);
  CleanupLogFile(log_file_3);
}