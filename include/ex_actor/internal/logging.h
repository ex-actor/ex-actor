// Copyright 2025 The ex_actor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <iostream>
#include <memory>
#include <source_location>
#include <sstream>
#include <iomanip>
#include <exception>

#include <rfl/to_view.hpp>
#include <spdlog/spdlog.h>
#include "ex_actor/internal/alias.h"

namespace ex_actor {
enum class LogLevel : uint8_t {
  kDebug = 0,
  kInfo = 1,
  kWarn = 2,
  kError = 3,
  kFatal = 4,
};

struct LogConfig {
  LogLevel level = LogLevel::kInfo;
  // empty means print to stdout
  std::string log_file_path;
};
}  // namespace ex_actor

namespace ex_actor::internal {
inline constexpr char kDefaultLoggerPattern[] = "[%^%L%$%Y-%m-%d %T.%e%z %P/%t %s:%#] %v";

spdlog::level::level_enum ToSpdlogLevel(LogLevel level);

std::unique_ptr<spdlog::logger> CreateLoggerUsingConfig(const LogConfig& config);

std::unique_ptr<spdlog::logger>& GlobalLogger();

void InstallFallbackExceptionHandler();

template <typename T>
concept Enum = std::is_enum_v<T>;

template <Enum E>
std::ostream& operator<<(std::ostream& ostream, E enum_v) {
  return ostream << static_cast<std::underlying_type_t<E>>(enum_v);
}

struct ThrowStream : public std::exception {
 public:
  template <typename U>
  ThrowStream&& operator<<(const U& val) && {
    ss_ << val;
    return std::move(*this);
  }

  const char* what() const noexcept override {
    what_ = ss_.str();
    return what_.c_str();
  }

  ThrowStream() = default;
  ThrowStream(const ThrowStream& rhs) { ss_ << rhs.ss_.str(); }
  ThrowStream(ThrowStream&& rhs) noexcept = default;

 private:
  std::stringstream ss_;
  mutable std::string what_;
};

#define EXA_THROW throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " "

#define EXA_THROW_IF(condition) \
  if (condition) [[unlikely]]   \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " `" << #condition << "` "

#define EXA_THROW_CHECK(condition)          \
  if (!(condition)) [[unlikely]]            \
  throw ::ex_actor::internal::ThrowStream() \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #condition << "` is true, got false. "

#define EXA_THROW_CHECK_EQ(val1, val2)                                                                             \
  if ((val1) != (val2)) [[unlikely]]                                                                               \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 \
                                            << " == " << #val2 << "`, got " << (val1) << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_LE(val1, val2)                                                                             \
  if ((val1) > (val2)) [[unlikely]]                                                                                \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 \
                                            << " <= " << #val2 << "`, got " << (val1) << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_LT(val1, val2)                                                                             \
  if ((val1) >= (val2)) [[unlikely]]                                                                               \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 \
                                            << " < " << #val2 << "`, got " << (val1) << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_GE(val1, val2)                                                                             \
  if ((val1) < (val2)) [[unlikely]]                                                                                \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 \
                                            << " >= " << #val2 << "`, got " << (val1) << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_GT(val1, val2)                                                                             \
  if ((val1) <= (val2)) [[unlikely]]                                                                               \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 \
                                            << " > " << #val2 << "`, got " << (val1) << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_NE(val1, val2)                                                                             \
  if ((val1) == (val2)) [[unlikely]]                                                                               \
  throw ::ex_actor::internal::ThrowStream() << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 \
                                            << " != " << #val2 << "`, got " << (val1) << " vs " << (val2) << ". "

template <class T>
concept HasOstreamOperator = requires(T t, std::ostream& os) {
  { os << t } -> std::same_as<std::ostream&>;
};

void ReflectPrintToStream(std::ostream& os, const auto& obj) {
  auto view = rfl::to_view(obj);
  view.apply([&os](const auto& field) { os << field.name() << "=" << *field.value() << ","; });
}

template <typename T, typename... Args>
std::string JoinVarsNameValue(std::string_view names, T&& first, Args&&... remaining) {
  std::ostringstream builder;

  // find variable end
  auto end = names.find_first_of(',');

  // display one variable
  if constexpr (HasOstreamOperator<decltype(first)>) {
    builder << names.substr(0, end) << "=" << first;
  } else {
    builder << names.substr(0, end) << "=";
    ReflectPrintToStream(builder, first);
  }

  if constexpr (sizeof...(Args) > 0) {
    // recursively call with the new beginning for names
    builder << "," << JoinVarsNameValue(names.substr(end + 1), std::forward<Args>(remaining)...);
  }

  return builder.str();
}

#define EXA_DUMP_VARS(...) ::ex_actor::internal::JoinVarsNameValue(#__VA_ARGS__, __VA_ARGS__)

}  // namespace ex_actor::internal

namespace ex_actor::internal::log {

inline spdlog::source_loc ToSpdlogSourceLoc(const std::source_location& loc) {
  return spdlog::source_loc {loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
}

// A format string wrapper that also captures source_location at the call site.
// This avoids the GCC limitation where source_location can't follow a parameter pack.
template <typename... Args>
struct FormatWithLoc {
  spdlog::format_string_t<Args...> fmt;
  std::source_location loc;

  template <typename T>
  consteval FormatWithLoc(  // NOLINT(google-explicit-constructor)
      const T& s, const std::source_location& loc = std::source_location::current())
      : fmt(s), loc(loc) {}
};

template <typename... Args>
void Info(FormatWithLoc<std::type_identity_t<Args>...> fmt_with_loc, Args&&... args) {
  internal::GlobalLogger()->log(ToSpdlogSourceLoc(fmt_with_loc.loc), spdlog::level::info, fmt_with_loc.fmt,
                                std::forward<Args>(args)...);
}

template <typename... Args>
void Warn(FormatWithLoc<std::type_identity_t<Args>...> fmt_with_loc, Args&&... args) {
  internal::GlobalLogger()->log(ToSpdlogSourceLoc(fmt_with_loc.loc), spdlog::level::warn, fmt_with_loc.fmt,
                                std::forward<Args>(args)...);
}

template <typename... Args>
void Error(FormatWithLoc<std::type_identity_t<Args>...> fmt_with_loc, Args&&... args) {
  internal::GlobalLogger()->log(ToSpdlogSourceLoc(fmt_with_loc.loc), spdlog::level::err, fmt_with_loc.fmt,
                                std::forward<Args>(args)...);
}

template <typename... Args>
void Critical(FormatWithLoc<std::type_identity_t<Args>...> fmt_with_loc, Args&&... args) {
  internal::GlobalLogger()->log(ToSpdlogSourceLoc(fmt_with_loc.loc), spdlog::level::critical, fmt_with_loc.fmt,
                                std::forward<Args>(args)...);
}

struct DebugInfo {
  uint64_t trace_id = 0;
};

/**
 * @brief Structured data representing an exception in the actor system.
 * Carries the original error message and the accumulated distributed stack trace.
 */
struct Frame {
  std::string method_name;
  std::optional<std::string> user_context;
  std::string file;
  uint32_t line = 0;

  std::string ToString() const {
    std::string short_file = file;
    // Heuristic: relative paths from common root directories
    for (const char* p : {"/test/", "/src/", "/include/"}) {
      size_t pos = file.find(p);
      if (pos != std::string::npos) {
        short_file = file.substr(pos + 1);
        break;
      }
    }

    auto location = (!short_file.empty()) ? fmt_lib::format(" @ {}:{}", short_file, line) : "";
    auto context = user_context.has_value() && !user_context->empty()
                       ? fmt_lib::format(" ({})", *user_context)
                       : "";
    return fmt_lib::format("{}{}{}", method_name, context, location);
  }
};

struct ActorExceptionData {
  uint64_t trace_id = 0;
  std::string original_what;
  std::string original_type;
  std::vector<Frame> stack_trace;
};

class ActorException : public std::exception {
 public:
  explicit ActorException(ActorExceptionData data) : data_(std::move(data)) {
    UpdateMessage();
  }

  const char* what() const noexcept override { return msg_.c_str(); }
  const ActorExceptionData& GetData() const { return data_; }

  void AppendFrame(std::string_view method_name, 
                   std::optional<std::string_view> user_context = std::nullopt,
                   std::string_view file = "", uint32_t line = 0) {
    data_.stack_trace.push_back(Frame{
        .method_name = std::string(method_name),
        .user_context = user_context.has_value() 
                         ? std::make_optional(std::string(*user_context)) 
                         : std::nullopt,
        .file = std::string(file),
        .line = line});
    UpdateMessage();
  }

 private:
  void UpdateMessage() {
    std::ostringstream oss;
    oss << "ActorException: [" << data_.original_type << "] " << data_.original_what << "\n";
    oss << "Trace:";
    for (size_t i = 0; i < data_.stack_trace.size(); ++i) {
      oss << "\n  #" << std::left << std::setw(2) << i << data_.stack_trace[i].ToString();
    }
    msg_ = oss.str();
  }

  ActorExceptionData data_;
  std::string msg_;
};

std::shared_ptr<const DebugInfo> GetCurrentDebugInfo();
void SetCurrentDebugInfo(std::shared_ptr<const DebugInfo> info);
void ClearCurrentDebugInfo();
std::shared_ptr<const DebugInfo> CreateNewDebugInfo();

/**
 * @brief 
 * Deprecated as we shift towards zero-overhead forward tracing.
 */
[[deprecated("Use SendBuilder::AttachDebugInfo instead")]] 
void LogAttachDebugInfo(std::string_view message, std::source_location loc = std::source_location::current());

}  // namespace ex_actor::internal::log

// Backward-compatibility aliases — these namespaces were removed in favor of ex_actor.
// Use ex_actor::LogLevel and ex_actor::LogConfig directly.
namespace ex_actor::logging {
using LogLevel [[deprecated("Use ex_actor::LogLevel instead")]] = ex_actor::LogLevel;
using LogConfig [[deprecated("Use ex_actor::LogConfig instead")]] = ex_actor::LogConfig;
}  // namespace ex_actor::logging