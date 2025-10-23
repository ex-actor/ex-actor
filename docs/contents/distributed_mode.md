# Distributed Mode

!!! experimental

    This feature is still in early stage. Bugs and API changes should be expected. Welcome to have a try and build together with us!


Distributed mode enables you to create actors at remote nodes. When calling a remote actor, all arguments will be
serialized and send to the remote node through network, after execution the return value will be deserialized and send
back to the caller.

Due to the lack of reflection before C++26, some boilerplate is needed.

1. Add a static `Create` method to you class: `static YourClass Create(...)`.
2. Add a `constexpr static std::tuple kActorMethods` to you class, which contains all methods you want to call remotely.
The name can be adjusted by defining `EXA_ACTOR_METHODS_KEYWORD`.
3. List all actor classes in a `ex_actor::ActorRoster<A, B, C...>` and pass it to `ex_actor::ActorRegistry`.

They are used to create a fixed ID for class and its constructor&methods, so that we know how to serialize/deserialize.

**✨ All these boilerplate can be eliminated in C++26 using reflection, I'll implement it when C++26 is released, stay tuned!**

## Example

<!-- doc test start, wrapper_script: test/multi_process_test.sh -->
```cpp
#include <cassert>

#include "ex_actor/api.h"

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  // Boilerplate 1, the Create method
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

  // Boilerplate 2, the kActorMethods tuple.
  // The name can be adjusted by defining `EXA_ACTOR_METHODS_KEYWORD`
  static constexpr std::tuple kActorMethods = {&PingWorker::Ping};

 private:
  std::string name_;
};

int main(int argc, char** argv) {
  uint32_t this_node_id = std::atoi(argv[1]);
  ex_actor::WorkSharingThreadPool thread_pool(4);

  // Boilerplate 3, the actor roster(a name list of all actors)
  // split by comma, e.g. ActorRoster<A, B, C...>
  ex_actor::ActorRoster<PingWorker> roster;

  std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                       {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
  ex_actor::ActorRegistry registry(thread_pool.GetScheduler(),
                                   /*this_node_id=*/this_node_id, cluster_node_info, roster);

  uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();
  auto ping_worker =
      registry.CreateActorUseStaticCreateFn<PingWorker>(ex_actor::ActorConfig {.node_id = remote_node_id},
                                                        /*name=*/"Alice");
  auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
  auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
  assert(ping_res == "ack from Alice, msg got: hello");
}
```
<!-- doc test end -->
## Serialization

We choose [reflect-cpp](https://github.com/getml/reflect-cpp) as the serialization library.
It is a reflection-based C++20 serialization library, which can serialize basic structs automatically, avoid a lot of boilerplate code.

As for the protocol, since we have a fixed schema by nature(all nodes use the same binary, so the types are the same across all nodes),
we can take advantage of a schemafull protocol. From this reason, we choose [Cap'n Proto](https://capnproto.org/) from reflect-cpp's supported protocols.


## Network

We choose [ZeroMQ](https://zeromq.org/), it's a well-known and sophisticated message passing library.

The topology is a full mesh. Each node holds one receive DEALER socket bound to local. And several send DEALER sockets connected to other nodes.

While full mesh is simple and efficient in small cluster, it has a potential scalability issue, because the number of connections is O(n^2).
It fits my current use case, so I have no plan yet to optimize further.
If you met scalability issue, you can try to use a different topology, e.g. star topology.
With ZeroMQ you can easily implement it by adding a central broker. Welcome to contribute!