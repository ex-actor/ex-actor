# ![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white) ex_actor

[![License: Apache2.0](https://img.shields.io/badge/License-Apache2.0-blue.svg)](https://opensource.org/licenses/apache-2.0)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](docs/contents/assets/ex_actor_banner.jpg)

**📖 [Documentation](https://ex-actor.github.io/ex-actor/)**

**ex_actor** is a modern C++ [actor framework](https://en.wikipedia.org/wiki/Actor_model) based on `std::execution`. **Only requires C++20 [(?)](#faqs)**.

This framework turns your C++ class into an async service. You can easily write distributed applications with it, without caring about thread synchronization and network.

Key Features:

1. **Easy to Use** - Turn your existing class into an actor. No arcane macros and inheritance.
2. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also provide some out-of-box, e.g. work-sharing & work-stealing thread pool.
3. **Standard-Compliant API** - Our actor returns a standard sender, compatible with everything in the std::execution ecosystem. You can `co_await` it, use `ex::then` to wrap etc.


# API Glance

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

struct YourClass {
  int Add(int x) { return count += x; }
  int count = 0;
};

exec::task<int> Test() {
  // 1. Choose a std::execution scheduler you like.
  ex_actor::WorkSharingThreadPool thread_pool(10);
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler());

  // 2. Create the actor.
  ex_actor::ActorRef actor = registry.CreateActor<YourClass>();

  // 3. Call it! It returns a standard sender.
  auto sender = actor.Send<&YourClass::Add>(1) 
                | stdexec::then([](int value) { return value + 1; });
  co_return co_await sender;
}

int main() {
  auto [res] = stdexec::sync_wait(Test()).value();
  assert(res == 2);
}
```
<!-- doc test end -->

**Check our 📖 [Documentation](https://ex-actor.github.io/ex-actor/) for more details like [installation](https://ex-actor.github.io/ex-actor/installation/) & [tutorials](https://ex-actor.github.io/ex-actor/tutorial/).**

# FAQs

## `std::execution` is in C++26, why you only requires C++20?

C++26 is not finalized, now we depends on an early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), which only requires C++20. (it's like `fmtlib` vs `std::format` and `ranges-v3` vs `std::ranges`)

Once C++26 is ready, we'll add a build option to switch to the real `std::execution` in C++26, allow you to remove the dependency on `stdexec`. And it'll only be an option, you can still use `stdexec` because all senders based on `stdexec` will work well with `std::execution`.

**From our side, we'll keep our code compatible with both `stdexec` and `std::execution`**. So don't worry about the dependency.

BTW, with C++26's reflection, most boilerplate of the distributed mode API can be eliminated. I'll add a new set of APIs for C++26 in the future, stay tuned!

## Is it production-ready?

The single-process mode is heavily tested in our company's production environment. Feel free to use it.

The distributed mode is still in early stage. Welcome to have a try and build together with us!

# The Team Behind `ex_actor`

We are ...