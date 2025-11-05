# ![C++](https://img.shields.io/badge/c++-%2300599C.svg?style=for-the-badge&logo=c%2B%2B&logoColor=white) ex_actor

[![License: Apache2.0](https://img.shields.io/badge/License-Apache2.0-blue.svg)](https://opensource.org/licenses/apache-2.0)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/)

![image](docs/contents/assets/ex_actor_banner.jpg)

**📖 [Documentation](https://ex-actor.github.io/ex-actor/)**

**ex_actor** is a modern C++ [actor framework](https://en.wikipedia.org/wiki/Actor_model) based on `std::execution`. **Only requires C++20 [(?)](#faqs)**.

This framework turns you C++ class into a stateful async service(so-called actor). All method calls to it will be pushed into the mailbox(a FIFO queue) of the actor and executed sequentially.

The actor can be local or remote. When it's local, args in method are passed directly in memory. When it's a remote actor, we'll help you to serialize them, send through network, and get the return value back.

This programming model is called "Actor Model". **It offers a simple way to build highly parallel programs — no locks, no race conditions — just write plain classes.**

[This video](https://www.youtube.com/watch?v=ELwEdb_pD0k) gives a good introduction to the Actor Model from high level.

# Key Features

1. **Easy to Use** - Non-intrusive API, turn your existing class into an actor. No arcane macros and inheritance.
2. **Pluggable Scheduler** - Use any std::execution scheduler you like! We also provide many out-of-box: work-sharing, work-stealing, custom priority and so on.
3. **Standard-Compliant API** - Our actor returns a standard `std::execution::task`, compatible with everything in the std::execution ecosystem. You can `co_await` it, use `ex::then` to wrap etc.


# API Glance

Note: currently we're based on std::execution's early implementation - [stdexec](https://github.com/NVIDIA/stdexec),
so you'll see namespaces like `stdexec` and `exec` instead of `std::execution` in the following example.

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

  // 3. Call it! It returns a `std::execution::task`.
  int res = co_await actor.Send<&YourClass::Add>(1);
  co_return res + 1;
}

int main() {
  auto [res] = stdexec::sync_wait(Test()).value();
  assert(res == 2);
}
```
<!-- doc test end -->

**Check 📘[Tutorials](https://ex-actor.github.io/ex-actor/tutorial/) page for more examples!**

## Installation

See ⚙️[Installation](https://ex-actor.github.io/ex-actor/installation/) page in our documentation.

## Contributing

### Have questions about any part of the project?

Participating in this project is not limited to writing code, your usage and feedback are also invaluable contributions!

If you find any bugs or have any feature requests, open an issue to let us know, we'll try our best to help you.

### Want to implement something yourself?

Have a look at existing issues and pick what you're interested in, especially those with "good first issue" label, such issues are relatively easy to implement.

For more guides, see [contributing](https://ex-actor.github.io/ex-actor/contributing/) page for more details like [how to build from source](https://ex-actor.github.io/ex-actor/contributing/#how-to-build-from-source).

# FAQs

## `std::execution` is in C++26, why you only requires C++20?

C++26 is not finalized, now we depends on an early implementation of `std::execution` - [nvidia/stdexec](https://github.com/NVIDIA/stdexec), which only requires C++20. (it's like `fmtlib` vs `std::format` and `ranges-v3` vs `std::ranges`)

Once C++26 is ready, we'll add a build option to switch to the real `std::execution` in C++26, allow you to remove the dependency on `stdexec`. And it'll only be an option, you can still use `stdexec` because all senders based on `stdexec` will work well with `std::execution`.

**From our side, we'll keep our code compatible with both `stdexec` and `std::execution`**. So don't worry about the dependency.

BTW, with C++26's reflection, most boilerplate of the distributed mode API can be eliminated. I'll add a new set of APIs for C++26 in the future, stay tuned!

## Is it production-ready?

The single-process mode is heavily tested in our company's production environment. While minor bugs can occur due to version divergence btw open source & internal codes, the overall quality is good, feel free to use it in production.

The distributed mode is still in early stage. Welcome to have a try and build together with us!

## The Team Behind `ex_actor`

We are [Beijing Qianxiang Private Fund Management Co., Ltd (乾象投资)](https://www.qianxiang.cn/), founded in 2018, a technology-driven investment management firm with a focus on artificial intelligence and machine learning. We deeply integrate and enhance machine learning algorithms, applying them to financial data characterized by extremely low signal-to-noise ratios, and thereby generating sustainable returns with low risks for our investors.

In the process of engineering our trading system, we discovered that no existing actor framework on the market could meet our specific requirements. Consequently, we built one from the ground up.

While this framework has some successful applications internally, we believe there are more valuable use case in the community which can make it more mature. So we open-source ex_actor, look forward to building it together with you!
