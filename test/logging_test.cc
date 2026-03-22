#include "ex_actor/internal/logging.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

// Test 1: Start() and Stop() without configure logging, should see all logs
TEST(LoggingTest, StartStopWithoutConfigureLogging) {
  std::string log_file = "test_log_1.txt";

  // Clean up any existing log file
  CleanupLogFile(log_file);

  // Configure logging to file with default Info level
  ex_actor::ConfigureLogging({
      .log_file_path = log_file,
  });

  // Start and Stop
  stdexec::sync_wait(ex_actor::Start(4));
  stdexec::sync_wait(ex_actor::Stop());

  // flush log
  ex_actor::internal::GlobalLogger()->flush();

  // Read log file and verify both Start and Stop logs are present
  std::string log_contents = ReadFile(log_file);
  EXPECT_FALSE(log_contents.empty()) << "Log file should not be empty";
  EXPECT_TRUE(Contains(log_contents, "Initializing ex_actor")) << "Should see Start log. Log contents:\n"
                                                               << log_contents;
  EXPECT_TRUE(Contains(log_contents, "Shutting down ex_actor")) << "Should see Stop log. Log contents:\n"
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

  // Start and Stop - these produce Info level logs
  stdexec::sync_wait(ex_actor::Start(4));
  stdexec::sync_wait(ex_actor::Stop());

  // flush log
  ex_actor::internal::GlobalLogger()->flush();

  // Read log file - should be empty or not contain Info logs
  std::string log_contents = ReadFile(log_file);
  EXPECT_FALSE(Contains(log_contents, "Initializing ex_actor"))
      << "Should NOT see Start log at Error level. Log contents:\n"
      << log_contents;
  EXPECT_FALSE(Contains(log_contents, "Shutting down ex_actor"))
      << "Should NOT see Stop log at Error level. Log contents:\n"
      << log_contents;

  // Clean up
  CleanupLogFile(log_file);
}

// Test 3: Start() first with Info level, then ConfigureLogging() with Error level in the middle,
// and Stop(). Should only see Start log, not Stop log
TEST(LoggingTest, ConfigureLoggingInMiddle) {
  std::string log_file = "test_log_3.txt";

  // Clean up any existing log file
  CleanupLogFile(log_file);

  // Configure logging to file with Info level initially
  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kInfo,
      .log_file_path = log_file,
  });

  // Start - should be logged
  stdexec::sync_wait(ex_actor::Start(4));

  // Change log level to Error in the middle
  ex_actor::ConfigureLogging({
      .level = ex_actor::LogLevel::kError,
      .log_file_path = log_file,
  });

  // Stop - should NOT be logged because level is now Error
  stdexec::sync_wait(ex_actor::Stop());

  // flush log
  ex_actor::internal::GlobalLogger()->flush();

  // Read log file
  std::string log_contents = ReadFile(log_file);
  EXPECT_FALSE(log_contents.empty()) << "Log file should not be empty";
  EXPECT_TRUE(Contains(log_contents, "Initializing ex_actor"))
      << "Should see Start log (before level change). Log contents:\n"
      << log_contents;
  EXPECT_FALSE(Contains(log_contents, "Shutting down ex_actor"))
      << "Should NOT see Stop log (after level change to Error). Log contents:\n"
      << log_contents;

  // Clean up
  CleanupLogFile(log_file);
}