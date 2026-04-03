#include "ex_actor/internal/logging.h"

#include <spdlog/spdlog.h>

#include <random>

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

std::unique_ptr<spdlog::logger>& GlobalLogger() {
  static std::unique_ptr<spdlog::logger> global_logger = CreateLoggerUsingConfig({});
  return global_logger;
}

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

namespace log {
thread_local std::shared_ptr<const DebugInfo> kCurrentDebugInfo;
std::shared_ptr<const DebugInfo> GetCurrentDebugInfo() { return kCurrentDebugInfo; }
void SetCurrentDebugInfo(std::shared_ptr<const DebugInfo> info) { kCurrentDebugInfo = std::move(info); }
void ClearCurrentDebugInfo() { kCurrentDebugInfo.reset(); }

std::shared_ptr<const DebugInfo> CreateNewDebugInfo() {
  static thread_local std::mt19937_64 engine(std::random_device {}());
  return std::make_shared<const DebugInfo>(DebugInfo {.trace_id = engine()});
}

void LogAttachDebugInfo(std::string_view message, std::source_location loc) {
  internal::GlobalLogger()->log(ToSpdlogSourceLoc(loc), spdlog::level::info, "DebugInfo (Legacy): {}", message);
}
}  // namespace log

}  // namespace ex_actor::internal