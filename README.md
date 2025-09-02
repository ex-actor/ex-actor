# ![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white) ex_actor

[![License: Apache](https://img.shields.io/badge/License-Apache-blue.svg)](https://opensource.org/licenses/MIT)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](assets/ex_actor_banner.png)

**ex_actor** is a modern C++ [actor framework](https://en.wikipedia.org/wiki/Actor_model) based on `std::execution`, **only requires C++20 ([detail](#requirements-and-dependencies))**.

An actor framework turns your class into a remote server - 'actor'. All method calls will be queued to the actor's mailbox, and executed sequentially. You can easily write distributed applications with it, without caring about thread synchronization and network.

Key Features:
1. **Easy to Use** - Make your class an actor by **one line**. No arcane macros and templates.
2. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also provide many out-of-box: work stealing, work sharing, custom priority...
3. **Sender-Based API** - Compatible with everything in the std::execution ecosystem - you can `co_await` it, pass to async algorithms, transfer to other execution resources and so on.


# Example

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

# Requirements and Dependencies

C++26 is not finalized, now we depends on an early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), which only requires C++20.

Once C++26 is ready, we'll add a check for your C++ version, and switch dependencies automatically:
* If your compiler supports C++26, we'll switch to the real `std::execution`, and enable some fancy reflection-based APIs.
* If not, we'll keep using `nvidia/stdexec`. Don't worry we won't give up C++20 :)

# How to Get `ex_actor`

We provide examples of different build systems, check them at [`test/import_test`](test/import_test).

* For CMake project:
  * [Use CMake Package Manager (CPM)](test/import_test/cmake_cpm) (recommended)
  * [Install & find_package](test/import_test/cmake_install_find_package)
* For Bazel project, you can use [rules_foreign_cc](https://bazel-contrib.github.io/rules_foreign_cc/cmake.html):
  * [Bzlmod sytle](test/import_test/bazel_bzlmod)
  * [Legacy WORKSPACE style](test/import_test/bazel_workspace)

Can't find your build system? Open an issue to let us know. Welcome to open a PR to contribute!

# Thread Model
