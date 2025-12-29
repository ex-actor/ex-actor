
[![License: Apache2.0](https://img.shields.io/badge/License-Apache2.0-blue.svg)](https://opensource.org/licenses/apache-2.0)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)
[![Build and test](https://github.com/ex-actor/ex-actor/actions/workflows/cmake_multi_platform.yml/badge.svg)](https://github.com/ex-actor/ex-actor/actions/workflows/cmake_multi_platform.yml)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/ex-actor/ex-actor)

<!-- GITHUB README ONLY START -->
**📖 Documentation: <https://ex-actor.github.io/ex-actor/>**
<!-- GITHUB README ONLY END -->

![image](assets/ex_actor_banner.png)

**ex_actor** is a modern C++ actor framework based on `std::execution`. **Only requires C++20 [(?)](#faqs)**.

This framework **turns your C++ class into a stateful async service** (so called actor) by one line of code. All method calls to it will be queued and executed in serial, so in your class you don't need any lock, just focus on your logic.

When the actor is local, args in method will be passed directly in memory. When it's a remote actor, it'll help you to serialize them, send through network, and get the return value back.

This programming paradigm is called "Actor Model". It's a very easy way to write distributed & highly concurrent programs, because you don't need any locks in your code. Just write plain classes.

# Key Features

1. **Easy to Use** - Non-intrusive API, turn your existing class into an actor, no need to modify your class.
2. **Standard-Compliant** - Composible with everything in std::execution ecosystem.
3. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also [provide many out-of-box](https://ex-actor.github.io/ex-actor/schedulers/): work-sharing, work-stealing, custom priority and so on.


# API Glance

**📘 Full Tutorial : <https://ex-actor.github.io/ex-actor/tutorial/>**

Note: currently we're based on std::execution's early implementation - [stdexec](https://github.com/NVIDIA/stdexec),
so you'll see namespaces like `stdexec` and `exec` instead of `std::execution` in the following example.

## Basic Example: Turn Your Class into an Actor

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

struct Counter {
  int Add(int x) { return count += x; }
  int count = 0;
};

exec::task<void> MainCoroutine() {
  ex_actor::Init(/*thread_pool_size=*/1);

  // 1. Create the actor.
  ex_actor::ActorRef actor = co_await ex_actor::Spawn<Counter>();

  // 2. Call it! It returns a `std::execution::task`.
  int res = co_await actor.Send<&Counter::Add>(1);
  assert(res == 1);
}

int main() { stdexec::sync_wait(MainCoroutine()); }
```
<!-- doc test end -->

## Actor Chaining Example

Actor's method can be a coroutine. The following example shows how to create an actor inside an actor and call it without blocking the scheduler thread.

<!-- doc test start -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

class Child {
public:
  std::string Ping() {
    return "Dad, I'm here!";
  }
};

class Father {
public:
  // actor's method can be a coroutine
  exec::task<std::string> SpawnChildAndPing() {
    if (child_.IsEmpty()) {
      child_ = co_await ex_actor::Spawn<Child>();
    }
    std::string child_res = co_await child_.Send<&Child::Ping>();
    co_return "Where is my child? " + child_res;
  }
private:
  ex_actor::ActorRef<Child> child_;
};

exec::task<void> MainCoroutine() {
  // Here we have only one thread in scheduler, but it still can finish the entire work
  // because we use coroutine, there is no blocking wait in actor's method.
  ex_actor::Init(/*thread_pool_size=*/1);

  ex_actor::ActorRef<Father> father = co_await ex_actor::Spawn<Father>();
  std::string res = co_await father.Send<&Father::SpawnChildAndPing>();
  assert(res == "Where is my child? Dad, I'm here!");
}

int main() { stdexec::sync_wait(MainCoroutine()); }
```
<!-- doc test end -->

**📘 Full Tutorial : <https://ex-actor.github.io/ex-actor/tutorial/>**

## Installation

See ⚙️[Installation](https://ex-actor.github.io/ex-actor/installation/) page in our documentation.

## Learning Resources for Actor Model

 - [Actor Model Explained (Video, English)](https://www.youtube.com/watch?v=ELwEdb_pD0k)
 - [Wikipedia - Actor Model](https://en.wikipedia.org/wiki/Actor_model)
 - [Talk at PureCpp 2025 (Video, Chinese)](https://www.bilibili.com/video/BV1LYB7BMEeQ/)

# FAQs

## `std::execution` is in C++26, why does it only require C++20?

C++26 is not finalized, now we depend on an early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), which only requires C++20. (it's like `fmtlib` vs `std::format` and `ranges-v3` vs `std::ranges`)

Once C++26 is ready, we'll add a build option to switch to the real `std::execution` in C++26, allowing you to remove the dependency on `stdexec`. And it'll only be an option, you can still use `stdexec` because all senders based on `stdexec` will work well with `std::execution`.

**From our side, we'll keep our code compatible with both `stdexec` and `std::execution`**. So don't worry about the dependency.

BTW, with C++26's reflection, most boilerplate of the distributed mode API can be eliminated. I'll add a new set of APIs for C++26 in the future, stay tuned!

## Is it production-ready?

The single-process mode is heavily tested in our company's production environment(Linux). While minor bugs can occur due to version divergence between open source & internal codes, the overall quality is good, feel free to use it in production.

The distributed mode's feature is complete and ready to be used, but isn't massively tested in production yet. Welcome to have a try and build together with us!

## The Team Behind `ex_actor`

We are
<img src="assets/metabit_logo.png" alt="Logo" style="height:1em;">
[Beijing Qianxiang Private Fund Management Co., Ltd (乾象投资)](https://www.qianxiang.cn/),
founded in 2018, a technology-driven investment management firm with a focus on artificial intelligence and machine learning. We deeply integrate and enhance machine learning algorithms, applying them to financial data characterized by extremely low signal-to-noise ratios, and thereby generating sustainable returns with low risks for our investors.

In the process of engineering our trading system, we discovered that no existing actor framework on the market could meet our specific requirements. Consequently, we built one from the ground up.

While this framework has some successful applications internally, we believe there are more valuable use cases in the community which can make it more mature. So we open-source ex_actor, look forward to building it together with you!
