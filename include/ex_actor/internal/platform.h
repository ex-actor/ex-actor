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

#include <string>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ex_actor::internal {

#if defined(__linux__)
inline bool SetThreadName(const std::string& name) { return pthread_setname_np(pthread_self(), name.c_str()) == 0; }
inline bool SetThreadAffinity(size_t core_index) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_index, &cpuset);
  return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}
#else
inline void SetThreadName(const std::string&) {}
inline bool SetThreadAffinity(size_t) { return false; }

#endif

}  // namespace ex_actor::internal
