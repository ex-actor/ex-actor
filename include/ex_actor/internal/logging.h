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
#include <sstream>

#include <rfl/to_view.hpp>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <spdlog/sinks/wincolor_sink.h>
#else
#include <spdlog/sinks/ansicolor_sink.h>
#endif

#if __cpp_lib_stacktrace >= 202011L
#include <stacktrace>
#endif

namespace ex_actor::internal::logging {

inline std::shared_ptr<spdlog::logger> CreateLogger(const std::string& name) {
#ifdef _WIN32
  auto color_sink = std::make_shared<spdlog::sinks::wincolor_stdout_sink_mt>();
#else
  auto color_sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
#endif
  auto logger = std::make_shared<spdlog::logger>(name, std::move(color_sink));
  logger->set_pattern("[%Y-%m-%d %T.%e%z] [%^%L%$] [%t] %v");
  return logger;
}

inline void InstallFallbackExceptionHandler() {
  std::set_terminate([] {
    if (auto ex = std::current_exception()) {
      try {
        std::rethrow_exception(ex);
      } catch (const std::exception& e) {
        spdlog::critical("terminate called with an active exception, type: {}, what: {}", typeid(e).name(), e.what());
      } catch (...) {
        spdlog::critical("terminate called with an unknown exception");
      }
    } else {
      spdlog::critical("terminate called without an active exception");
    }
#if __cpp_lib_stacktrace >= 202011L
    spdlog::info("backtrace:\n{}", std::to_string(std::stacktrace::current()));
#endif
    std::abort();
  });
};

inline void SetupProcessWideLoggingConfig() {
  static std::atomic_bool is_setup = false;
  bool expected = false;
  bool changed = is_setup.compare_exchange_strong(expected, true);
  if (!changed) {
    return;
  }
  spdlog::set_pattern("[%Y-%m-%d %T.%e%z] [%^%L%$] [%t] %v");
  InstallFallbackExceptionHandler();
}

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

#define EXA_THROW throw ::ex_actor::internal::logging::ThrowStream() << __FILE__ << ":" << __LINE__ << " "

#define EXA_THROW_IF(condition) \
  if (condition) [[unlikely]]   \
  throw ::ex_actor::internal::logging::ThrowStream() << __FILE__ << ":" << __LINE__ << " `" << #condition << "` "

#define EXA_THROW_CHECK(condition)                   \
  if (!(condition)) [[unlikely]]                     \
  throw ::ex_actor::internal::logging::ThrowStream() \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #condition << "` is true, got false. "

#define EXA_THROW_CHECK_EQ(val1, val2)                                                                                 \
  if ((val1) != (val2)) [[unlikely]]                                                                                   \
  throw ::ex_actor::internal::logging::ThrowStream()                                                                   \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " == " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_LE(val1, val2)                                                                                 \
  if ((val1) > (val2)) [[unlikely]]                                                                                    \
  throw ::ex_actor::internal::logging::ThrowStream()                                                                   \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " <= " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_LT(val1, val2)                                                                                \
  if ((val1) >= (val2)) [[unlikely]]                                                                                  \
  throw ::ex_actor::internal::logging::ThrowStream()                                                                  \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " < " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_GE(val1, val2)                                                                                 \
  if ((val1) < (val2)) [[unlikely]]                                                                                    \
  throw ::ex_actor::internal::logging::ThrowStream()                                                                   \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " >= " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_GT(val1, val2)                                                                                \
  if ((val1) <= (val2)) [[unlikely]]                                                                                  \
  throw ::ex_actor::internal::logging::ThrowStream()                                                                  \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " > " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_NE(val1, val2)                                                                                 \
  if ((val1) == (val2)) [[unlikely]]                                                                                   \
  throw ::ex_actor::internal::logging::ThrowStream()                                                                   \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " != " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

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

#define EXA_DUMP_VARS(...) ::ex_actor::internal::logging::JoinVarsNameValue(#__VA_ARGS__, __VA_ARGS__)
}  // namespace ex_actor::internal::logging