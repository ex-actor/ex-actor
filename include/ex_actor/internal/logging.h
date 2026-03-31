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

#include <rfl/to_view.hpp>
#include <spdlog/spdlog.h>

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

/**
 * @brief Special log function for AttachDebugInfo feature.
 */
inline void LogAttachDebugInfo(std::string_view message, std::source_location loc) {
  internal::GlobalLogger()->log(ToSpdlogSourceLoc(loc), spdlog::level::info, "DebugInfo: {}", message);
}

}  // namespace ex_actor::internal::log

// Backward-compatibility aliases — these namespaces were removed in favor of ex_actor.
// Use ex_actor::LogLevel and ex_actor::LogConfig directly.
namespace ex_actor::logging {
using LogLevel [[deprecated("Use ex_actor::LogLevel instead")]] = ex_actor::LogLevel;
using LogConfig [[deprecated("Use ex_actor::LogConfig instead")]] = ex_actor::LogConfig;
}  // namespace ex_actor::logging