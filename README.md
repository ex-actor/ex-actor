# ![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white) ex_actor

[![License: Apache](https://img.shields.io/badge/License-Apache-blue.svg)](https://opensource.org/licenses/MIT)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](assets/ex_actor_banner.png)

**ex_actor** is a modern actor framework based on `std::execution`, **but only requires C++20**.

> [!NOTE]
> C++26 is not finalized, now we are based on an early implementation - [nvidia/stdexec](https://github.com/NVIDIA/stdexec).
> Once C++26 is ready, we'll switch to the real std::execution, while keep using `nvidia/stdexec` to keep C++20 compatibility - don't worry we won't give up C++20 :)

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

Check the examples of different build systems in [test/import_test](test/import_test). They are tested in CI for every commit.

* For CMake project:
  * [Use CMake Package Manager (CPM)](test/import_test/cmake_cpm)
  * [Install & find_package](test/import_test/cmake_find_package)
* For Bazel project, you can use [rules_foreign_cc](https://bazel-contrib.github.io/rules_foreign_cc/cmake.html):
  * [Legacy WORKSPACE style](test/import_test/bazel_legacy_workspace)
  * [Bzlmod sytle](test/import_test/bazel_bzlmod)

Can't find your build system? Open an issue to let us know. Welcome to open a PR to contribute!