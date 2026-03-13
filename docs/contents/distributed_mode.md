# Distributed Mode

!!! Note

    This feature is currently experimental. While it's fully functional and ready for use, it's not massively tested in production yet. Bugs, performance issues and API changes should be expected. Welcome to have a try and build together with us!


Distributed mode enables you to create actors at remote nodes. When calling a remote actor, all arguments will be
serialized and sent to the remote node through network, after execution the return value will be deserialized and sent
back to the caller.

## Make your actor class be able to be created remotely

To make your actor class be able to be created remotely, you need to use `EXA_REMOTE` macro to register the class:

```cpp
class YourClass {
 public:
  void Method1() {}
  void Method2() {}
  static YourClass Create() { return YourClass(); }
};

EXA_REMOTE(&YourClass::Create, &YourClass::Method1, &YourClass::Method2);

co_await ex_actor::Spawn<&YourClass::Create>().ToNode(node_id);
```

In the `EXA_REMOTE` macro, the first argument is a function to create your class, and the rest are methods you want to call remotely.
Then in `Spawn<>`, provide the factory function pointer (the first argument of `EXA_REMOTE`) as the template argument. This differs from local actors, where you would pass the class type itself (`Spawn<YourClass>()`).

Such boilerplate is caused by the lack of reflection before C++26. It can be simplified in C++26 using reflection, we'll add a new set of APIs for C++26 in the future, stay tuned!

## Start or join a cluster

To start a cluster, you need to pass a `ex_actor::ClusterConfig` to `ex_actor::Init()`.

```cpp
ex_actor::ClusterConfig cluster_config {
  .this_node_id = 19,
  // the public address other nodes use to connect to you.
  // we'll open a listening port at this address.
  .listen_address = "tcp://10.10.2.145:5301",
  // set to any node in the cluster to join an existing cluster.
  // for the first node, leave it empty.
  .contact_node_address = "tcp://10.10.2.113:5665",
};
ex_actor::Init(..., cluster_config);

co_await ex_actor::WaitNodeAlive(remote_node_id, /*timeout_ms=*/5000);
```

The cluster is dynamically connected, for the first node, leave the contact node address empty.
For the remaining nodes, set the contact node address to **any node** in the cluster to join.

Before operating with the remote node, call `ex_actor::WaitNodeAlive()` to confirm it is reachable; otherwise an exception will be thrown when you call `ex_actor::Spawn()` or `actor_ref.Send()`

## Example

<!-- doc test start, wrapper_script: test/multi_process_test.py -->
```cpp
#include <cassert>
#include <cstdlib>
#include <iostream>
#include "ex_actor/api.h"

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}

  // You can also put this outside the class if you don't want to modify your class
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};

// 0. Register the class & methods using EXA_REMOTE
EXA_REMOTE(&PingWorker::Create, &PingWorker::Ping);

// Usage: ./distributed_node <node_id> <listen_address> <remote_node_id> [contact_address]
// The first node in the cluster should omit contact_address.
exec::task<void> MainCoroutine(int argc, char** argv) {
  uint32_t this_node_id = std::atoi(argv[1]);
  std::string listen_address = argv[2];
  uint32_t remote_node_id = std::atoi(argv[3]);
  std::string contact_address = (argc > 4) ? argv[4] : "";
  ex_actor::ClusterConfig cluster_config {
      .this_node_id = this_node_id,
      .listen_address = listen_address,
      .contact_node_address = contact_address,
  };

  // 1. Start or join the cluster
  ex_actor::Init(/*thread_pool_size=*/4, cluster_config);

  // 2. Wait for the remote node to be alive
  bool connected = co_await ex_actor::WaitNodeAlive(remote_node_id, /*timeout_ms=*/5000);
  if (!connected) {
    throw std::runtime_error("Cannot connect to node " + std::to_string(remote_node_id));
  }

  // 3. Create a remote actor and send messages to it
  auto ping_worker =
      co_await ex_actor::Spawn<&PingWorker::Create>(/*name=*/"Alice").ToNode(remote_node_id);
  std::string ping_res = co_await ping_worker.Send<&PingWorker::Ping>("hello");
  assert(ping_res == "ack from Alice, msg got: hello");
  std::cout << "All work done, node id: " << this_node_id << std::endl;

  // Wait for OS exit signal(like CTRL+C or kill) before shutting down, otherwise the process
  // will exit immediately, which might be earlier than the other node finishes its work,
  // causing error in the other node.
  co_await ex_actor::WaitOsExitSignal();
  ex_actor::Shutdown();
}

int main(int argc, char** argv) { stdexec::sync_wait(MainCoroutine(argc, argv)); }
```
<!-- doc test end -->

Compile this program into a binary, let's say `distributed_node`.

usage: `./distributed_node <node_id> <listen_address> <remote_node_id> [contact_address]`

In one shell, run: `./distributed_node 0 tcp://127.0.0.1:5301 1`, in another shell, run: `./distributed_node 1 tcp://127.0.0.1:5302 0 tcp://127.0.0.1:5301`. Both processes should print "All work done" log.

Node 0 is the first node so it doesn't need a contact address. Node 1 specifies node 0's address as the contact address to join the cluster.

The process will block on `ex_actor::WaitOsExitSignal()` until OS exit signal is received. You should kill them manually by CTRL+C or kill command.


## Architecture details

### Serialization

We choose [reflect-cpp](https://github.com/getml/reflect-cpp) as the serialization library.
It is a reflection-based C++20 serialization library, which can serialize basic structs automatically, avoiding a lot of boilerplate code.

As for the protocol, since we have a fixed schema by nature(all nodes use the same binary, so the types are the same across all nodes),
we can take advantage of a schemafull protocol. For this reason, we choose [Cap'n Proto](https://capnproto.org/) from reflect-cpp's supported protocols.


### Network

We choose [ZeroMQ](https://zeromq.org/), it's a well-known and sophisticated message passing library.

The topology is a full mesh. Each node holds one receive DEALER socket bound locally and several send DEALER sockets connected to other nodes.

The node states are synchronized via gossip protocol, no centralized coordination node.