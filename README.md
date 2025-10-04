# ![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white) ex_actor

[![License: Apache2.0](https://img.shields.io/badge/License-Apache2.0-blue.svg)](https://opensource.org/licenses/apache-2.0)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](assets/ex_actor_banner.jpg)

**ex_actor** is a modern C++ [actor framework](https://en.wikipedia.org/wiki/Actor_model) following `std::execution`'s design. **Only requires C++20 ([detail](#faqs))**.

An actor framework turns your class into a remote service. All method calls will be queued to the actor's mailbox and executed sequentially. You can easily write distributed applications with it, without caring about thread synchronization and network.

Key Features:
1. **Easy to Use** - Make your class an actor by **one line**. No arcane macros and templates.
2. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also provide many out-of-box: work stealing, work sharing, custom priority...
3. **Standard-Compliant API** - Our actor returns a standard sender, compatible with everything in the std::execution ecosystem. You can `co_await` it, use `ex::then` to wrap etc.


# API Glance

```cpp
#include "ex_actor/api.h"
namespace ex = stdexec;

class Counter {
  public:
   int Add(int x) { return count_ += x; }
   
   // 1. Tell me your methods - the only line intrudes into your class.
   constexpr static auto kActorMethods = std::make_tuple(&Counter::Add);
 
  private:
   int count_ = 0;
 };
 
 exec::task<int> Test() {
   ex_actor::ActorRegistry registry;
 
   // 2. Create the actor. Use any std::execution scheduler you like!
   ex_actor::WorkSharingThreadPool thread_pool(10);
   ex_actor::ActorRef counter = registry.CreateActor<Counter>(thread_pool.GetScheduler());
 
   // 3. Call it! It returns a standard sender.
   auto sender = counter.Send<&Counter::Add>(1) 
                 | ex::then([](int value) { return value + 1; });
   co_return co_await sender;
 }
 
 int main() {
   auto [res] = ex::sync_wait(Test()).value();
   std::cout << "res: " << res << '\n';
   return 0;
 }
```

# How to Add `ex_actor` to Your Project

We provide examples of different build systems.

* For CMake project:
  * [Use CMake Package Manager (CPM)](test/import_test/cmake_cpm) (recommended)
  * [Install & find_package](test/import_test/cmake_install_find_package)
* For Bazel project:
  * [Bzlmod sytle](test/import_test/bazel_bzlmod)
  * [Legacy WORKSPACE style](test/import_test/bazel_workspace)

Can't find your build system? Open an issue to let us know. Welcome to open a PR to contribute!

# Dependencies

We know it's hard to resolve dependency conflicts, so we carefully choose minimal dependencies:

1. [stdexec](https://github.com/NVIDIA/stdexec)

For specific versions, please check [CMakeLists.txt](CMakeLists.txt), search for `CPMAddPackage`. If you meet any dependency conflict, please open an issue to let us know, we're happy to help.

# FAQs

## `std::execution` is in C++26, why you only requires C++20?

C++26 is not finalized, now we depends on an early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), which only requires C++20. (it's like `fmtlib` vs `std::format` and `ranges-v3` vs `std::ranges`)

Once C++26 is ready, we'll add a build option to switch to the real `std::execution` in C++26, allow you to remove the dependency on `stdexec`. And it'll only be an option, you can still use `stdexec` because all senders based on `stdexec` will work well with `std::execution`.

**From our side, we'll keep our code compatible with both `stdexec` and `std::execution`**. So don't worry about the dependency.

## Is it production-ready?

The single-process mode is heavily tested in our company's production environment. Feel free to use it.

The distributed mode is still in early stage. Welcome to have a try and give us feedback!

# The Team Behind `ex_actor`

We are ...