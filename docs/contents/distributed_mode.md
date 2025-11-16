# Distributed Mode

!!! experimental

    This feature is still in early stage. Bugs and API changes should be expected. Welcome to have a try and build together with us!


Distributed mode enables you to create actors at remote nodes. When calling a remote actor, all arguments will be
serialized and send to the remote node through network, after execution the return value will be deserialized and send
back to the caller.

To make your actor class be able to be created remotely, you need to use `EXA_REMOTE` macro to register the class, e.g.:

```cpp
class YourClass  {
 public:
  void Method1() {};
  void Method2() {};
};

YourClass FactoryCreate() { return YourClass(); }
EXA_REMOTE(&YourClass::FactoryCreate, &YourClass::Method1, &YourClass::Method2);
```

In the `EXA_REMOTE` macro, the first argument is a factory function to create your class, and the rest are the methods you want to call remotely.

This is used to generate a serialization schema for network communication.

Then instead of calling `registry.CreateActor<YourClass>()`, you need to call `registry.CreateActor<YourClass, &FactoryCreate>()`. Or an exception will be thrown.

✨ **BTW, this can be simplified in C++26 using reflection, I'll implement it when C++26 is released, stay tuned!** ✨

## Example

<!-- doc test start, wrapper_script: test/multi_process_test.sh -->
```cpp
#include <cassert>
#include "ex_actor/api.h"

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  // You can also put this outside the class if you don't want to modify your class
  static PingWorker FactoryCreate(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};

// 1. Register the class & methods using EXA_REMOTE
EXA_REMOTE(&PingWorker::FactoryCreate, &PingWorker::Ping);

int main(int argc, char** argv) {
  uint32_t this_node_id = std::atoi(argv[1]);

  std::vector<ex_actor::NodeInfo> cluster_node_info = {{.node_id = 0, .address = "tcp://127.0.0.1:5301"},
                                                       {.node_id = 1, .address = "tcp://127.0.0.1:5302"}};
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/4,
                                   /*this_node_id=*/this_node_id, cluster_node_info);

  uint32_t remote_node_id = (this_node_id + 1) % cluster_node_info.size();

  // 2. Specify the factory function in registry.CreateActor
  auto ping_worker = registry.CreateActor<PingWorker, &PingWorker::FactoryCreate>(
      ex_actor::ActorConfig {.node_id = remote_node_id}, /*name=*/"Alice"
  );
  auto ping = ping_worker.Send<&PingWorker::Ping>("hello");
  auto [ping_res] = stdexec::sync_wait(std::move(ping)).value();
  assert(ping_res == "ack from Alice, msg got: hello");
}
```
<!-- doc test end -->

Compile this program into a binary, let's say `distributed_node`.

In one shell, run: `./distributed_node 0`, in another shell, run: `./distributed_node 1`. Both processes should exit normally.

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