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

#include "ex_actor/3rd_lib/absl/base/internal/throw_delegate.h"

#include <cstdlib>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>

namespace ex_actor::embedded_3rd::absl {
EX_ACTOR_ABSL_NAMESPACE_BEGIN
namespace base_internal {

void ThrowStdLogicError(const std::string& what_arg) { throw std::logic_error(what_arg); }
void ThrowStdLogicError(const char* what_arg) { throw std::logic_error(what_arg); }
void ThrowStdInvalidArgument(const std::string& what_arg) { throw std::invalid_argument(what_arg); }
void ThrowStdInvalidArgument(const char* what_arg) { throw std::invalid_argument(what_arg); }
void ThrowStdDomainError(const std::string& what_arg) { throw std::domain_error(what_arg); }
void ThrowStdDomainError(const char* what_arg) { throw std::domain_error(what_arg); }
void ThrowStdLengthError(const std::string& what_arg) { throw std::length_error(what_arg); }
void ThrowStdLengthError(const char* what_arg) { throw std::length_error(what_arg); }
void ThrowStdOutOfRange(const std::string& what_arg) { throw std::out_of_range(what_arg); }
void ThrowStdOutOfRange(const char* what_arg) { throw std::out_of_range(what_arg); }
void ThrowStdRuntimeError(const std::string& what_arg) { throw std::runtime_error(what_arg); }
void ThrowStdRuntimeError(const char* what_arg) { throw std::runtime_error(what_arg); }
void ThrowStdRangeError(const std::string& what_arg) { throw std::range_error(what_arg); }
void ThrowStdRangeError(const char* what_arg) { throw std::range_error(what_arg); }
void ThrowStdOverflowError(const std::string& what_arg) { throw std::overflow_error(what_arg); }
void ThrowStdOverflowError(const char* what_arg) { throw std::overflow_error(what_arg); }
void ThrowStdUnderflowError(const std::string& what_arg) { throw std::underflow_error(what_arg); }
void ThrowStdUnderflowError(const char* what_arg) { throw std::underflow_error(what_arg); }
void ThrowStdBadFunctionCall() { throw std::bad_function_call(); }
void ThrowStdBadAlloc() { throw std::bad_alloc(); }

}  // namespace base_internal
EX_ACTOR_ABSL_NAMESPACE_END
}  // namespace ex_actor::embedded_3rd::absl
