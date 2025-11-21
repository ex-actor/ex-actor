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

#include <exception>
#include <iostream>
#include <sstream>
#include <utility>

#include <cpptrace/cpptrace.hpp>
#include <rfl/to_view.hpp>
#include <spdlog/spdlog.h>

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#include "ex_actor/internal/alias.h"

namespace ex_actor::internal::logging {

inline bool IsTerminal() {
#if __has_include(<unistd.h>)
  return isatty(1) == 1;
#else
  return false;
#endif
}

template <typename T>
concept Enum = std::is_enum_v<T>;

template <Enum E>
std::ostream& operator<<(std::ostream& ostream, E enum_v) {
  return ostream << static_cast<std::underlying_type_t<E>>(enum_v);
}

inline std::string FormatStackTrace(const cpptrace::stacktrace& stacktrace, int num_spaces_prepend = 2) {
  std::stringstream ss;
  bool color = IsTerminal();
  for (const auto& frame : stacktrace.frames) {
    ss << std::string(num_spaces_prepend, ' ') << frame.to_string(color) << '\n';
  }
  return ss.str();
}

class ExceptionWithStacktrace : public std::exception {
 public:
  explicit ExceptionWithStacktrace(const std::string& message,
                                   cpptrace::stacktrace stacktrace = cpptrace::stacktrace::current())
      : stacktrace_(std::move(stacktrace)) {
    message_stream_ << message;
  }

  explicit ExceptionWithStacktrace(const std::exception_ptr& exception_ptr, cpptrace::stacktrace stacktrace)
      : stacktrace_(std::move(stacktrace)) {
    try {
      std::rethrow_exception(exception_ptr);
    } catch (const std::exception& e) {
      message_stream_ << fmt_lib::format("{{Wrapped plain exception, type: {}, what: {}}}", typeid(e).name(), e.what());
    } catch (...) {
      message_stream_ << "unknown exception";
    }
  }

  const char* what() const noexcept override {
    if (!what_.empty()) {
      return what_.c_str();
    }
    what_ = fmt_lib::format("ExceptionWithStacktrace, message: {}, stack trace:\n{}", message_stream_.str(),
                            FormatStackTrace(stacktrace_));
    return what_.c_str();
  }

  template <typename U>
  ExceptionWithStacktrace&& operator<<(const U& val) && {
    message_stream_ << val;
    return std::move(*this);
  }

 protected:
  std::stringstream message_stream_;
  cpptrace::stacktrace stacktrace_;
  mutable std::string what_;
};

class NestableException : public ExceptionWithStacktrace {
 public:
  explicit NestableException(const std::string& message, std::exception_ptr caused_by = nullptr)
      : ExceptionWithStacktrace(message), caused_by_(std::move(caused_by)) {}

  NestableException(const std::string& message, std::string specified_caused_by_message)
      : ExceptionWithStacktrace(message), specified_caused_by_message_(std::move(specified_caused_by_message)) {}

  // NOLINTNEXTLINE(misc-no-recursion)
  const char* what() const noexcept override {
    if (!what_.empty()) {
      return what_.c_str();
    }
    std::string caused_by_str = GetCausedByMessage();
    if (caused_by_str.empty()) {
      what_ = fmt_lib::format("NestableException, message: {}, stack trace:\n{}", message_stream_.str(),
                              FormatStackTrace(stacktrace_));
    } else {
      what_ = fmt_lib::format("NestableException, message: {}, stack trace:\n{}\n--->Cased by: {}",
                              message_stream_.str(), FormatStackTrace(stacktrace_), caused_by_str);
    }
    return what_.c_str();
  }

 private:
  std::exception_ptr caused_by_;

  std::string specified_caused_by_message_;
  mutable std::string what_;

  // NOLINTNEXTLINE(misc-no-recursion)
  std::string GetCausedByMessage() const {
    if (!specified_caused_by_message_.empty()) {
      return specified_caused_by_message_;
    }
    if (caused_by_ == nullptr) {
      return {};
    }
    std::string caused_by_message;
    try {
      std::rethrow_exception(caused_by_);
    } catch (const ExceptionWithStacktrace& e) {
      caused_by_message = e.what();
    } catch (const std::exception& e) {
      caused_by_message = fmt_lib::format("Exception type: {}, what: {}", typeid(e).name(), e.what());
    } catch (...) {
      caused_by_message = "unknown exception";
    }
    return caused_by_message;
  }
};

#define EXA_THROW throw ::ex_actor::internal::logging::ExceptionWithStacktrace("") << __FILE__ << ":" << __LINE__ << " "

#define EXA_THROW_IF(condition)                                    \
  if (condition) [[unlikely]]                                      \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("") \
      << __FILE__ << ":" << __LINE__ << " `" << #condition << "` "

#define EXA_THROW_CHECK(condition)                                 \
  if (!(condition)) [[unlikely]]                                   \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("") \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #condition << "` is true, got false. "

#define EXA_THROW_CHECK_EQ(val1, val2)                                                                                 \
  if ((val1) != (val2)) [[unlikely]]                                                                                   \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("")                                                     \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " == " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_LE(val1, val2)                                                                                 \
  if ((val1) > (val2)) [[unlikely]]                                                                                    \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("")                                                     \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " <= " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_LT(val1, val2)                                                                                \
  if ((val1) >= (val2)) [[unlikely]]                                                                                  \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("")                                                    \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " < " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_GE(val1, val2)                                                                                 \
  if ((val1) < (val2)) [[unlikely]]                                                                                    \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("")                                                     \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " >= " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_GT(val1, val2)                                                                                \
  if ((val1) <= (val2)) [[unlikely]]                                                                                  \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("")                                                    \
      << __FILE__ << ":" << __LINE__ << " Check failed, expected `" << #val1 << " > " << #val2 << "`, got " << (val1) \
      << " vs " << (val2) << ". "

#define EXA_THROW_CHECK_NE(val1, val2)                                                                                 \
  if ((val1) == (val2)) [[unlikely]]                                                                                   \
  throw ::ex_actor::internal::logging::ExceptionWithStacktrace("")                                                     \
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

inline void InstallFallbackExceptionHandler() {
  std::set_terminate([] {
    if (auto ex = std::current_exception()) {
      NestableException error("terminate called with an active exception", ex);
      spdlog::critical("{}", error.what());
    } else {
      NestableException error("terminate called without an active exception");
      spdlog::critical("{}", error.what());
    }
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
}  // namespace ex_actor::internal::logging