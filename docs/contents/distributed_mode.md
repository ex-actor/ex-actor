# Distributed Mode

!!! Note

    This feature is currently experimental. While it's fully functional and ready for use, it's not massively tested in production yet. Bugs, performance issues and API changes should be expected. Welcome to have a try and build together with us!


Distributed mode enables you to create actors at remote nodes. When calling a remote actor, all arguments will be
serialized and send to the remote node through network, after execution the return value will be deserialized and send
back to the caller.

## Make your actor class be able to be created remotely
To make your actor class be able to be created remotely, you need to use `EXA_REMOTE` macro to register the class, e.g.:

```cpp
class YourClass  {
 public:
  void Method1() {};
  void Method2() {};
};

YourClass CreateFn() { return YourClass(); }
EXA_REMOTE(&CreateFn, &YourClass::Method1, &YourClass::Method2);
```

In the `EXA_REMOTE` macro, the first argument is a factory function to create your class, and the rest are the methods you want to call remotely.

This is used to generate a serialization schema for network communication.

Then instead of calling `ex_actor::Spawn<YourClass>()`, you need to call `ex_actor::Spawn<&CreateFn>()`, and specify the target node using `.ToNode(node_id)`.

Such boilerplate is caused by the lack of reflection before C++26. It can be simplified in C++26 using reflection, we'll add a new set of APIs for C++26 in the future, stay tuned!

## Start or join a cluster

To start a cluster, you need to pass a `ex_actor::ClusterConfig` to `ex_actor::Init()`.

```cpp
ex_actor::ClusterConfig cluster_config {
  .this_node_id = this_node_id,
  .listen_address = listen_address,
  // For first node, leave it empty. For other nodes, set to any node in the cluster to join.
  .contact_node_address = contact_address, 
};
ex_actor::Init(..., cluster_config);

co_await ex_actor::WaitNodeAlive(remote_node_id, /*timeout_ms=*/5000);
```

The cluster is dynamically connected, for the first node, leave the contact node address empty.
For the other nodes, set the contact node address to **any node** in the cluster to join.

Before operating with the remote node, you should call `ex_actor::WaitNodeAlive()` to wait for the remote node to be alive.
Or an exception will be thrown when you call `ex_actor::Spawn()` or `actor_ref.Send()`

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
  static PingWorker CreateFn(std::string name) { return PingWorker(std::move(name)); }

  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }

 private:
  std::string name_;
};

// 0. Register the class & methods using EXA_REMOTE
EXA_REMOTE(&PingWorker::CreateFn, &PingWorker::Ping);

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
      co_await ex_actor::Spawn<&PingWorker::CreateFn>(/*name=*/"Alice").ToNode(remote_node_id);
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

## Serialization

We choose [reflect-cpp](https://github.com/getml/reflect-cpp) as the serialization library.
It is a reflection-based C++20 serialization library, which can serialize basic structs automatically, avoid a lot of boilerplate code.

As for the protocol, since we have a fixed schema by nature(all nodes use the same binary, so the types are the same across all nodes),
we can take advantage of a schemafull protocol. From this reason, we choose [Cap'n Proto](https://capnproto.org/) from reflect-cpp's supported protocols.


## Network

We choose [ZeroMQ](https://zeromq.org/), it's a well-known and sophisticated message passing library.

The topology is a full mesh. Each node holds one receive DEALER socket bound to local and several send DEALER sockets connected to other nodes.

The node states are synchronized via gossip protocol, no centralized coordination node.