# ![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white) ex_actor

[![License: Apache](https://img.shields.io/badge/License-Apache-blue.svg)](https://opensource.org/licenses/MIT)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](assets/ex_actor_banner.png)

**ex_actor** is a modern actor framework based on `std::execution`, **but only requires C++20**.

C++26 is not finalized, now we are based on an early implementation - [nvidia/stdexec](https://github.com/NVIDIA/stdexec).
Once C++26 is ready, we'll switch to the real std::execution, while keep using `nvidia/stdexec` to keep C++20 compatibility - don't worry we won't give up C++20 :)

Key Features:
1. **Easy to Use** - Make your class an actor by **one line**. No arcane macros and templates.
2. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also provide many out-of-box: work stealing, work sharing, custom priority...
3. **Sender-Based API** - Compatible with everything in the std::execution ecosystem - you can `co_await` it, pass to async algorithms, transfer to other execution resources and so on.


# Quick Start

```cpp
#include "ex_actor/api.h"

class Counter {
 public:
  int Add(int x) { return count_ += x; }
  
  // Tell me your methods - it's all you need to make your class an actor.
  constexpr static auto kActorMethods = std::make_tuple(&Counter::Add);

 private:
  int count_ = 0;
};

exec::task<void> TestBasicUseCase() {
  ex_actor::ActorRegistry registry;

  // Use any std::execution scheduler you like!
  ex_actor::WorkStealingThreadPool thread_pool(10);
  ex_actor::ActorRef counter = registry.CreateActor<Counter>(thread_pool.get_scheduler()); 

  // Coroutine support!
  std::cout << co_await counter.Call<&Counter::Add>(1) << '\n';
}

int main() {
  stdexec::sync_wait(TestBasicUseCase());
  return 0;
}
```

# Using `ex_actor`

## CMake Projects

### Use [CMake Package Manager (CPM)](https://github.com/cpm-cmake/CPM.cmake)
```cmake
cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)
project(your_project)

# For more information on how to add CPM to your project, see: https://github.com/cpm-cmake/CPM.cmake#adding-cpm
include(CPM.cmake)

CPMAddPackage(
  NAME ex_actor
  GITHUB_REPOSITORY ex-actor/ex-actor
  GIT_TAG main # This will always pull the latest code from the `main` branch. You may also use a specific commit ID
  OPTIONS "EX_ACTOR_BUILD_TESTS OFF"
)

add_executable(main example.cpp)
target_link_libraries(main ex_actor::ex_actor)
```

### CMake Install & find_package

First install ex_actor to your system
```bash
git clone --depth=1 https://github.com/ex-actor/ex-actor.git && cd ex-actor
./regen_build_dir.sh && cd build
cmake --build . --config Release
cmake --install . --config Release --prefix=<your install path>
```

Then include to your project using find_package

```cmake
find_package(ex_actor)
if(ex_actor_FOUND)
    message(STATUS "ex_actor include dir: ${ex_actor_INCLUDE_DIRS}")
    message(STATUS "ex_actor libraries: ${ex_actor_LIBRARIES}")
endif()
add_executable(MyApp main.cpp)
target_include_directories(MyApp PRIVATE ${ex_actor_INCLUDE_DIRS})
target_link_libraries(MyApp PRIVATE ${ex_actor_LIBRARIES})
```

## Bazel Projects

Use [rules_foreign_cc](https://bazel-contrib.github.io/rules_foreign_cc/cmake.html)

WORKSPACE:
```python
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(
    name = "ex_actor",
    # This will always pulls the latest source code, you may need to set a fixed version
    urls = ["https://github.com/ex-actor/ex-actor/archive/refs/heads/main.zip"],
    # Run sha256sum on the downloaded file to get it
    sha256 = "...",
)
```

BUILD file:
```python
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")

cmake(
    name = "ex_actor",
    lib_source = "@ex_actor//:all_srcs",
    generate_args = ["-GNinja"],
)
```