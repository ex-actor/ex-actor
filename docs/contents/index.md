# Introduction

[![License: Apache2.0](https://img.shields.io/badge/License-Apache2.0-blue.svg)](https://opensource.org/licenses/apache-2.0)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](assets/ex_actor_banner.jpg)

**ex_actor** is a modern C++ actor framework based on `std::execution`. **Only requires C++20 [(?)](#faqs)**.

This framework **turns you C++ class into a stateful async service**(so-called actor). All method calls to it will be pushed into the mailbox(a FIFO queue) of the actor and executed sequentially.

The actor can be local or remote. When it's local, args in method are passed directly in memory. When it's a remote actor, we'll help you to serialize them, send through network, and get the return value back.

**It offers a simple way to build highly concurrent programs — no locks, no race conditions — just write plain classes.**

# Key Features

1. **Easy to Use** - Non-intrusive API, turn your existing class into an actor, no need to modify your class.
2. **Standard-Compliant API** - Our actor returns a standard `std::execution::task`, compatible with everything in the std::execution ecosystem. You can `co_await` it, use `ex::then` to wrap etc.
3. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also provide many out-of-box: work-sharing, work-stealing, custom priority and so on, see [docs](https://ex-actor.github.io/ex-actor/schedulers/).


# API Glance

Note: currently we're based on std::execution's early implementation - [stdexec](https://github.com/NVIDIA/stdexec),
so you'll see namespaces like `stdexec` and `exec` instead of `std::execution` in the following example.

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

struct Counter {
  int Add(int x) { return count += x; }
  int count = 0;
};

exec::task<void> MainCoroutine() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/10);

  // 1. Create the actor.
  ex_actor::ActorRef actor = co_await registry.CreateActor<Counter>();

  // 2. Call it! It returns a `std::execution::task`.
  int res = co_await actor.Send<&Counter::Add>(1);
  assert(res == 1);
}

int main() { stdexec::sync_wait(MainCoroutine()); }
```
<!-- doc test end -->

## Next Steps

1. How to add `ex_actor` to your project? - [Installation](installation.md)
2. How to use `ex_actor`? - [Tutorial](tutorial.md)

## FAQs

### `std::execution` is in C++26, why you only requires C++20?

C++26 is not finalized, now we depends on an early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), which only requires C++20. (it's like `fmtlib` vs `std::format` and `ranges-v3` vs `std::ranges`)

Once C++26 is ready, we'll add a build option to switch to the real `std::execution` in C++26, allow you to remove the dependency on `stdexec`. And it'll only be an option, you can still use `stdexec` because all senders based on `stdexec` will work well with `std::execution`.

**From our side, we'll keep our code compatible with both `stdexec` and `std::execution`**. So don't worry about the dependency.

BTW, with C++26's reflection, most boilerplate of the distributed mode API can be eliminated. I'll add a new set of APIs for C++26 in the future, stay tuned!

### Is it production-ready?

The single-process mode is heavily tested in our company's production environment. While minor bugs can occur due to version divergence btw open source & internal codes, the overall quality is good, feel free to use it in production.

The distributed mode is still in early stage. Welcome to have a try and build together with us!

## Learning Resources

Theoretical learning resources for newbies to actor model:

 - [Actor Model Explained (Video)](https://www.youtube.com/watch?v=ELwEdb_pD0k)
 - [Wikipedia - Actor Model](https://en.wikipedia.org/wiki/Actor_model)

## The Team Behind `ex_actor`

We are [Beijing Qianxiang Private Fund Management Co., Ltd (乾象投资)](https://www.qianxiang.cn/), founded in 2018, a technology-driven investment management firm with a focus on artificial intelligence and machine learning. We deeply integrate and enhance machine learning algorithms, applying them to financial data characterized by extremely low signal-to-noise ratios, and thereby generating sustainable returns with low risks for our investors.

In the process of engineering our trading system, we discovered that no existing actor framework on the market could meet our specific requirements. Consequently, we built one from the ground up.

While this framework has some successful applications internally, we believe there are more valuable use case in the community which can make it more mature. So we open-source ex_actor, look forward to building it together with you!
