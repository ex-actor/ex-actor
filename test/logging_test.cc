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

struct SimpleStruct {
  int x;
  std::string name;
};

enum class TestEnum : int {
  kFirst = 1,
  kSecond = 2,
};

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

// --- Tests for ToSpdlogLevel ---

TEST(LoggingTest, ToSpdlogLevelDebug) {
  EXPECT_EQ(ex_actor::internal::ToSpdlogLevel(ex_actor::LogLevel::kDebug), spdlog::level::debug);
}

TEST(LoggingTest, ToSpdlogLevelInfo) {
  EXPECT_EQ(ex_actor::internal::ToSpdlogLevel(ex_actor::LogLevel::kInfo), spdlog::level::info);
}

TEST(LoggingTest, ToSpdlogLevelWarn) {
  EXPECT_EQ(ex_actor::internal::ToSpdlogLevel(ex_actor::LogLevel::kWarn), spdlog::level::warn);
}

TEST(LoggingTest, ToSpdlogLevelError) {
  EXPECT_EQ(ex_actor::internal::ToSpdlogLevel(ex_actor::LogLevel::kError), spdlog::level::err);
}

TEST(LoggingTest, ToSpdlogLevelFatal) {
  EXPECT_EQ(ex_actor::internal::ToSpdlogLevel(ex_actor::LogLevel::kFatal), spdlog::level::critical);
}

// --- Tests for CreateLoggerUsingConfig ---

TEST(LoggingTest, CreateLoggerWithStdout) {
  ex_actor::LogConfig config{.level = ex_actor::LogLevel::kInfo};
  auto logger = ex_actor::internal::CreateLoggerUsingConfig(config);
  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger->level(), spdlog::level::info);
}

TEST(LoggingTest, CreateLoggerWithFile) {
  std::string log_file = "test_create_logger.txt";
  CleanupLogFile(log_file);

  ex_actor::LogConfig config{.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file};
  auto logger = ex_actor::internal::CreateLoggerUsingConfig(config);
  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger->level(), spdlog::level::debug);

  // Write a message and verify it appears in the file
  logger->info("test message from CreateLoggerWithFile");
  logger->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_TRUE(Contains(contents, "test message from CreateLoggerWithFile"));

  // Clean up
  logger.reset();
  fs::remove(log_file);
}

TEST(LoggingTest, CreateLoggerWithDebugLevel) {
  ex_actor::LogConfig config{.level = ex_actor::LogLevel::kDebug};
  auto logger = ex_actor::internal::CreateLoggerUsingConfig(config);
  EXPECT_EQ(logger->level(), spdlog::level::debug);
}

TEST(LoggingTest, CreateLoggerWithWarnLevel) {
  ex_actor::LogConfig config{.level = ex_actor::LogLevel::kWarn};
  auto logger = ex_actor::internal::CreateLoggerUsingConfig(config);
  EXPECT_EQ(logger->level(), spdlog::level::warn);
}

// --- Tests for GlobalLogger ---

TEST(LoggingTest, GlobalLoggerIsNotNull) { EXPECT_NE(ex_actor::internal::GlobalLogger(), nullptr); }

TEST(LoggingTest, GlobalLoggerReturnsSameInstance) {
  auto& logger1 = ex_actor::internal::GlobalLogger();
  auto& logger2 = ex_actor::internal::GlobalLogger();
  EXPECT_EQ(&logger1, &logger2);
}

// --- Tests for ThrowStream ---

TEST(LoggingTest, ThrowStreamBasicMessage) {
  try {
    throw ex_actor::internal::ThrowStream() << "hello " << "world";
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    EXPECT_TRUE(Contains(e.what(), "hello world"));
  }
}

TEST(LoggingTest, ThrowStreamWithNumbers) {
  try {
    throw ex_actor::internal::ThrowStream() << "value=" << 42 << " pi=" << 3.14;
    FAIL() << "Should have thrown";
  } catch (const ex_actor::internal::ThrowStream& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "value=42"));
    EXPECT_TRUE(Contains(msg, "pi=3.14"));
  }
}

TEST(LoggingTest, ThrowStreamCopyConstructor) {
  ex_actor::internal::ThrowStream original;
  auto moved = std::move(original) << "original message";

  ex_actor::internal::ThrowStream copy(moved);
  EXPECT_TRUE(Contains(copy.what(), "original message"));
}

// --- Tests for EXA_THROW ---

TEST(LoggingTest, ExaThrowContainsFileAndLine) {
  try {
    EXA_THROW << "test error";
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "logging_test.cc"));
    EXPECT_TRUE(Contains(msg, "test error"));
  }
}

// --- Tests for EXA_THROW_IF ---

TEST(LoggingTest, ExaThrowIfTrueThrows) {
  EXPECT_THROW(
      {
        EXA_THROW_IF(true) << "condition was true";
      },
      ex_actor::internal::ThrowStream);
}

TEST(LoggingTest, ExaThrowIfFalseDoesNotThrow) {
  EXPECT_NO_THROW({
    EXA_THROW_IF(false) << "should not throw";
  });
}

TEST(LoggingTest, ExaThrowIfMessageContainsCondition) {
  try {
    int x = 5;
    EXA_THROW_IF(x > 3) << "extra info";
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "x > 3"));
    EXPECT_TRUE(Contains(msg, "extra info"));
  }
}

// --- Tests for EXA_THROW_CHECK ---

TEST(LoggingTest, ExaThrowCheckPassesOnTrue) {
  EXPECT_NO_THROW({ EXA_THROW_CHECK(1 == 1); });
}

TEST(LoggingTest, ExaThrowCheckThrowsOnFalse) {
  try {
    EXA_THROW_CHECK(1 == 2);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "1 == 2"));
  }
}

// --- Tests for EXA_THROW_CHECK_EQ ---

TEST(LoggingTest, ExaThrowCheckEqPassesOnEqual) {
  EXPECT_NO_THROW({ EXA_THROW_CHECK_EQ(3, 3); });
}

TEST(LoggingTest, ExaThrowCheckEqThrowsOnNotEqual) {
  try {
    int a = 3;
    int b = 5;
    EXA_THROW_CHECK_EQ(a, b);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "a == b"));
    EXPECT_TRUE(Contains(msg, "3"));
    EXPECT_TRUE(Contains(msg, "5"));
  }
}

// --- Tests for EXA_THROW_CHECK_NE ---

TEST(LoggingTest, ExaThrowCheckNePassesOnNotEqual) {
  EXPECT_NO_THROW({ EXA_THROW_CHECK_NE(3, 5); });
}

TEST(LoggingTest, ExaThrowCheckNeThrowsOnEqual) {
  try {
    int a = 7;
    EXA_THROW_CHECK_NE(a, 7);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "a != 7"));
    EXPECT_TRUE(Contains(msg, "7"));
  }
}

// --- Tests for EXA_THROW_CHECK_LE ---

TEST(LoggingTest, ExaThrowCheckLePassesOnLessOrEqual) {
  EXPECT_NO_THROW({
    EXA_THROW_CHECK_LE(3, 5);
    EXA_THROW_CHECK_LE(5, 5);
  });
}

TEST(LoggingTest, ExaThrowCheckLeThrowsOnGreater) {
  try {
    int a = 10;
    int b = 5;
    EXA_THROW_CHECK_LE(a, b);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "a <= b"));
    EXPECT_TRUE(Contains(msg, "10"));
    EXPECT_TRUE(Contains(msg, "5"));
  }
}

// --- Tests for EXA_THROW_CHECK_LT ---

TEST(LoggingTest, ExaThrowCheckLtPassesOnLess) {
  EXPECT_NO_THROW({ EXA_THROW_CHECK_LT(3, 5); });
}

TEST(LoggingTest, ExaThrowCheckLtThrowsOnGreaterOrEqual) {
  try {
    int a = 5;
    int b = 5;
    EXA_THROW_CHECK_LT(a, b);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "a < b"));
  }
}

// --- Tests for EXA_THROW_CHECK_GE ---

TEST(LoggingTest, ExaThrowCheckGePassesOnGreaterOrEqual) {
  EXPECT_NO_THROW({
    EXA_THROW_CHECK_GE(5, 3);
    EXA_THROW_CHECK_GE(5, 5);
  });
}

TEST(LoggingTest, ExaThrowCheckGeThrowsOnLess) {
  try {
    int a = 2;
    int b = 5;
    EXA_THROW_CHECK_GE(a, b);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "a >= b"));
    EXPECT_TRUE(Contains(msg, "2"));
    EXPECT_TRUE(Contains(msg, "5"));
  }
}

// --- Tests for EXA_THROW_CHECK_GT ---

TEST(LoggingTest, ExaThrowCheckGtPassesOnGreater) {
  EXPECT_NO_THROW({ EXA_THROW_CHECK_GT(5, 3); });
}

TEST(LoggingTest, ExaThrowCheckGtThrowsOnLessOrEqual) {
  try {
    int a = 5;
    int b = 5;
    EXA_THROW_CHECK_GT(a, b);
    FAIL() << "Should have thrown";
  } catch (const std::exception& e) {
    std::string msg = e.what();
    EXPECT_TRUE(Contains(msg, "Check failed"));
    EXPECT_TRUE(Contains(msg, "a > b"));
  }
}

// --- Tests for enum operator<< ---

TEST(LoggingTest, EnumOperatorStreamOutput) {
  std::ostringstream oss;
  ex_actor::internal::operator<<(oss, TestEnum::kFirst);
  EXPECT_EQ(oss.str(), "1");

  oss.str("");
  ex_actor::internal::operator<<(oss, TestEnum::kSecond);
  EXPECT_EQ(oss.str(), "2");
}

// --- Tests for EXA_DUMP_VARS ---

TEST(LoggingTest, DumpVarsSingleVariable) {
  int count = 42;
  std::string result = EXA_DUMP_VARS(count);
  EXPECT_TRUE(Contains(result, "count=42")) << "Got: " << result;
}

TEST(LoggingTest, DumpVarsMultipleVariables) {
  int x = 1;
  int y = 2;
  std::string name = "test";
  std::string result = EXA_DUMP_VARS(x, y, name);
  EXPECT_TRUE(Contains(result, "x=1")) << "Got: " << result;
  EXPECT_TRUE(Contains(result, "y=2")) << "Got: " << result;
  EXPECT_TRUE(Contains(result, "name=test")) << "Got: " << result;
}

TEST(LoggingTest, DumpVarsWithExpression) {
  int a = 3;
  int b = 4;
  std::string result = EXA_DUMP_VARS(a + b);
  EXPECT_TRUE(Contains(result, "a + b=7")) << "Got: " << result;
}

// --- Tests for ReflectPrintToStream ---

TEST(LoggingTest, ReflectPrintToStreamWithStruct) {
  SimpleStruct s{.x = 10, .name = "hello"};
  std::ostringstream oss;
  ex_actor::internal::ReflectPrintToStream(oss, s);
  std::string result = oss.str();
  EXPECT_TRUE(Contains(result, "x=10")) << "Got: " << result;
  EXPECT_TRUE(Contains(result, "name=hello")) << "Got: " << result;
}

TEST(LoggingTest, DumpVarsWithStruct) {
  SimpleStruct s{.x = 99, .name = "world"};
  std::string result = EXA_DUMP_VARS(s);
  EXPECT_TRUE(Contains(result, "s=")) << "Got: " << result;
  EXPECT_TRUE(Contains(result, "x=99")) << "Got: " << result;
  EXPECT_TRUE(Contains(result, "name=world")) << "Got: " << result;
}

// --- Tests for log::Info/Warn/Error/Critical ---

TEST(LoggingTest, LogInfoWritesToFile) {
  std::string log_file = "test_log_info.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file});

  ex_actor::internal::log::Info("info message {}", 123);
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_TRUE(Contains(contents, "info message 123")) << "Got: " << contents;

  CleanupLogFile(log_file);
}

TEST(LoggingTest, LogWarnWritesToFile) {
  std::string log_file = "test_log_warn.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file});

  ex_actor::internal::log::Warn("warn message {}", "abc");
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_TRUE(Contains(contents, "warn message abc")) << "Got: " << contents;

  CleanupLogFile(log_file);
}

TEST(LoggingTest, LogErrorWritesToFile) {
  std::string log_file = "test_log_error.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file});

  ex_actor::internal::log::Error("error message {}", 456);
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_TRUE(Contains(contents, "error message 456")) << "Got: " << contents;

  CleanupLogFile(log_file);
}

TEST(LoggingTest, LogCriticalWritesToFile) {
  std::string log_file = "test_log_critical.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file});

  ex_actor::internal::log::Critical("critical message {}", 789);
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_TRUE(Contains(contents, "critical message 789")) << "Got: " << contents;

  CleanupLogFile(log_file);
}

TEST(LoggingTest, LogLevelFilteringPreventsLowerLevelMessages) {
  std::string log_file = "test_log_filter.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kError, .log_file_path = log_file});

  ex_actor::internal::log::Info("should not appear");
  ex_actor::internal::log::Warn("also should not appear");
  ex_actor::internal::log::Error("should appear");
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_FALSE(Contains(contents, "should not appear")) << "Got: " << contents;
  EXPECT_FALSE(Contains(contents, "also should not appear")) << "Got: " << contents;
  EXPECT_TRUE(Contains(contents, "should appear")) << "Got: " << contents;

  CleanupLogFile(log_file);
}

// --- Tests for spdlog::fmt_lib::formatter<std::exception> ---

TEST(LoggingTest, ExceptionFormatterIncludesTypeAndWhat) {
  std::string log_file = "test_log_exception_fmt.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file});

  std::runtime_error err("something went wrong");
  const std::exception& base_ref = err;
  ex_actor::internal::log::Error("caught: {}", base_ref);
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  EXPECT_TRUE(Contains(contents, "something went wrong")) << "Got: " << contents;

  CleanupLogFile(log_file);
}

// --- Tests for InstallFallbackExceptionHandler ---

TEST(LoggingTest, InstallFallbackExceptionHandlerDoesNotCrash) {
  // Just verify it can be called without crashing.
  // We can't easily test the terminate handler behavior without forking,
  // since std::terminate aborts the process.
  EXPECT_NO_THROW({ ex_actor::internal::InstallFallbackExceptionHandler(); });
}

// --- Tests for log source location ---

TEST(LoggingTest, LogMessageContainsSourceLocation) {
  std::string log_file = "test_log_sourceloc.txt";
  CleanupLogFile(log_file);

  ex_actor::ConfigureLogging({.level = ex_actor::LogLevel::kDebug, .log_file_path = log_file});

  ex_actor::internal::log::Info("source location test");
  ex_actor::internal::GlobalLogger()->flush();

  std::string contents = ReadFile(log_file);
  // The log pattern includes %s:%# which is filename:line
  EXPECT_TRUE(Contains(contents, "logging_test.cc")) << "Got: " << contents;

  CleanupLogFile(log_file);
}