#include "ex_actor/internal/logging.h"

#include <atomic>
#include <spdlog/spdlog.h>

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace ex_actor::internal {
using ex_actor::LogLevel;

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return spdlog::level::debug;
    case LogLevel::kInfo:
      return spdlog::level::info;
    case LogLevel::kWarn:
      return spdlog::level::warn;
    case LogLevel::kError:
      return spdlog::level::err;
    case LogLevel::kFatal:
      return spdlog::level::critical;
  }
  EXA_THROW << "Invalid log level: " << level;
}

std::unique_ptr<spdlog::logger> CreateLoggerUsingConfig(const ex_actor::LogConfig& config) {
  constexpr char kLoggerName[] = "ex_actor";
  std::unique_ptr<spdlog::logger> logger;
  if (config.log_file_path.empty()) {
    logger = std::make_unique<spdlog::logger>(kLoggerName, std::make_unique<spdlog::sinks::stdout_color_sink_mt>());
  } else {
    logger = std::make_unique<spdlog::logger>(
        kLoggerName, std::make_unique<spdlog::sinks::basic_file_sink_mt>(config.log_file_path));
  }
  logger->set_level(ToSpdlogLevel(config.level));
  logger->set_pattern(kDefaultLoggerPattern);
  return logger;
}

// Thread-safe global logger using atomic shared_ptr
// This allows ConfigureLogging to atomically replace the logger while
// concurrent log operations continue using the old logger safely.
// The old logger is destroyed only when all references are released.
static std::atomic<std::shared_ptr<spdlog::logger>> g_global_logger {
  CreateLoggerUsingConfig({}).release()
};

std::shared_ptr<spdlog::logger> GlobalLogger() {
  return g_global_logger.load(std::memory_order_acquire);
}

namespace ex_actor {
void ConfigureLogging(const LogConfig& config) {
  g_global_logger.store(CreateLoggerUsingConfig(config).release(), std::memory_order_release);
}
}  // namespace ex_actor

void InstallFallbackExceptionHandler() {
  std::set_terminate([] {
    if (auto ex = std::current_exception()) {
      try {
        std::rethrow_exception(ex);
      } catch (const std::exception& e) {
        log::Critical("terminate called with an active exception, type: {}, what: {}", typeid(e).name(), e.what());
      } catch (...) {
        log::Critical("terminate called with an unknown exception");
      }
    } else {
      log::Critical("terminate called without an active exception");
    }
    std::abort();
  });
};
}  // namespace ex_actor::internal