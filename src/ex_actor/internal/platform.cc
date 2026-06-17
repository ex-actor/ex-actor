// Copyright 2026 The ex_actor Authors.
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

#include "ex_actor/internal/platform.h"

#include <string>

#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
// Supported platform — implementation below.
#else
#include "ex_actor/internal/logging.h"
#endif

#if defined(__linux__)

#include <pthread.h>
#include <sched.h>

namespace ex_actor::internal {

bool SetThreadName(const std::string& name) { return pthread_setname_np(pthread_self(), name.c_str()) == 0; }

bool SetThreadAffinity(size_t core_index) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_index, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

}  // namespace ex_actor::internal

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ex_actor::internal {

bool SetThreadName(const std::string& name) {
  int size = MultiByteToWideChar(CP_UTF8, /*dwFlags=*/0, name.c_str(), -1, nullptr, /*cchWideChar=*/0);
  if (size <= 0) {
    return false;
  }
  std::wstring wide(size, L'\0');
  MultiByteToWideChar(CP_UTF8, /*dwFlags=*/0, name.c_str(), -1, wide.data(), size);
  return SUCCEEDED(SetThreadDescription(GetCurrentThread(), wide.c_str()));
}

bool SetThreadAffinity(size_t core_index) {
  DWORD_PTR mask = 1ULL << core_index;
  return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}

}  // namespace ex_actor::internal

#elif defined(__APPLE__)

#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>

namespace ex_actor::internal {

bool SetThreadName(const std::string& name) { return pthread_setname_np(name.c_str()) == 0; }

bool SetThreadAffinity(size_t core_index) {
  // macOS doesn't support hard thread affinity. thread_affinity_policy is a
  // hint to the scheduler to co-locate threads with the same tag; it does NOT
  // pin to a specific core. We use the core_index as the affinity tag.
  thread_affinity_policy_data_t policy = {static_cast<integer_t>(core_index + 1)};
  return thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                           reinterpret_cast<thread_policy_t>(&policy),
                           THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
}

}  // namespace ex_actor::internal

#else

namespace ex_actor::internal {

bool SetThreadName(const std::string&) {
  log::Warn("SetThreadName is not supported on this platform");
  return false;
}

bool SetThreadAffinity(size_t) {
  log::Warn("SetThreadAffinity is not supported on this platform");
  return false;
}

}  // namespace ex_actor::internal

#endif
