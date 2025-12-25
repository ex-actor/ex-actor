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
  ex_actor::internal::logging::GlobalLogger()->flush();

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
      .level = ex_actor::logging::LogLevel::kError,
      .log_file_path = log_file,
  });

  // Init and Shutdown - these produce Info level logs
  ex_actor::Init(4);
  ex_actor::Shutdown();

  // flush log
  ex_actor::internal::logging::GlobalLogger()->flush();

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
      .level = ex_actor::logging::LogLevel::kInfo,
      .log_file_path = log_file,
  });

  // Init - should be logged
  ex_actor::Init(4);

  // Change log level to Error in the middle
  ex_actor::ConfigureLogging({
      .level = ex_actor::logging::LogLevel::kError,
      .log_file_path = log_file,
  });

  // Shutdown - should NOT be logged because level is now Error
  ex_actor::Shutdown();

  // flush log
  ex_actor::internal::logging::GlobalLogger()->flush();

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