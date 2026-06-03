//
// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Vendored inline variant: implementations are provided inline to keep this
// header-only. Only the subset actually used by absl::InlinedVector and
// absl::Span is needed.

#ifndef EX_ACTOR_ABSL_BASE_INTERNAL_THROW_DELEGATE_H_
#define EX_ACTOR_ABSL_BASE_INTERNAL_THROW_DELEGATE_H_

#include <functional>
#include <new>
#include <stdexcept>
#include <string>

#include "ex_actor/3rd_lib/absl/base/config.h"

namespace ex_actor::embedded_3rd::absl {
EX_ACTOR_ABSL_NAMESPACE_BEGIN
namespace base_internal {

[[noreturn]] inline void ThrowStdLogicError(const std::string& what_arg) { throw std::logic_error(what_arg); }
[[noreturn]] inline void ThrowStdLogicError(const char* what_arg) { throw std::logic_error(what_arg); }
[[noreturn]] inline void ThrowStdInvalidArgument(const std::string& what_arg) { throw std::invalid_argument(what_arg); }
[[noreturn]] inline void ThrowStdInvalidArgument(const char* what_arg) { throw std::invalid_argument(what_arg); }
[[noreturn]] inline void ThrowStdDomainError(const std::string& what_arg) { throw std::domain_error(what_arg); }
[[noreturn]] inline void ThrowStdDomainError(const char* what_arg) { throw std::domain_error(what_arg); }
[[noreturn]] inline void ThrowStdLengthError(const std::string& what_arg) { throw std::length_error(what_arg); }
[[noreturn]] inline void ThrowStdLengthError(const char* what_arg) { throw std::length_error(what_arg); }
[[noreturn]] inline void ThrowStdOutOfRange(const std::string& what_arg) { throw std::out_of_range(what_arg); }
[[noreturn]] inline void ThrowStdOutOfRange(const char* what_arg) { throw std::out_of_range(what_arg); }
[[noreturn]] inline void ThrowStdRuntimeError(const std::string& what_arg) { throw std::runtime_error(what_arg); }
[[noreturn]] inline void ThrowStdRuntimeError(const char* what_arg) { throw std::runtime_error(what_arg); }
[[noreturn]] inline void ThrowStdRangeError(const std::string& what_arg) { throw std::range_error(what_arg); }
[[noreturn]] inline void ThrowStdRangeError(const char* what_arg) { throw std::range_error(what_arg); }
[[noreturn]] inline void ThrowStdOverflowError(const std::string& what_arg) { throw std::overflow_error(what_arg); }
[[noreturn]] inline void ThrowStdOverflowError(const char* what_arg) { throw std::overflow_error(what_arg); }
[[noreturn]] inline void ThrowStdUnderflowError(const std::string& what_arg) { throw std::underflow_error(what_arg); }
[[noreturn]] inline void ThrowStdUnderflowError(const char* what_arg) { throw std::underflow_error(what_arg); }
[[noreturn]] inline void ThrowStdBadFunctionCall() { throw std::bad_function_call(); }
[[noreturn]] inline void ThrowStdBadAlloc() { throw std::bad_alloc(); }

}  // namespace base_internal
EX_ACTOR_ABSL_NAMESPACE_END
}  // namespace ex_actor::embedded_3rd::absl

#endif  // EX_ACTOR_ABSL_BASE_INTERNAL_THROW_DELEGATE_H_
