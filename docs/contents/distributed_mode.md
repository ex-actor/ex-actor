# Distributed Mode

!!! Note

    This feature is currently experimental. While it's fully functional and ready for use, it's not massively tested in production yet. Bugs, performance issues and API changes should be expected. Welcome to have a try and build together with us!

Distributed mode enables you to create actors at remote nodes. When calling a remote actor, all arguments will be
serialized and sent to the remote node through network, after execution the return value will be deserialized and sent
back to the caller.

## Example

<!-- doc test start, wrapper_script: test/multi_process_test.py -->
```cpp
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>
#include "ex_actor/api.h"

class PingWorker {
 public:
  explicit PingWorker(std::string name) : name_(std::move(name)) {}
  static PingWorker Create(std::string name) { return PingWorker(std::move(name)); }
  std::string Ping(const std::string& message) { return "ack from " + name_ + ", msg got: " + message; }
 private:
  std::string name_;
};

// 1. Register the class & methods using EXA_REMOTE macro.
// The first argument is a function to create your class,
// the rest are methods you want to call remotely.
EXA_REMOTE(&PingWorker::Create, &PingWorker::Ping); // (1)

exec::task<void> MainCoroutine(int argc, char** argv) {
  std::string listen_address = argv[1];
  std::string contact_address = (argc > 2) ? argv[2] : "";
  // 2. Init the framework, then start or join a cluster.
  ex_actor::Init(/*thread_pool_size=*/4);
  co_await ex_actor::StartOrJoinCluster(ex_actor::ClusterConfig {
      // The public address other nodes use to connect to you,
      // we'll open a listening port at this address.
      .listen_address = listen_address,
      // If you're the first node of the cluster, leave this empty.
      // Otherwise, set to any node in the cluster to join.
      .contact_node_address = contact_address,
  });

  // 3. Wait for the cluster to reach your desired state.
  auto [cluster_state, condition_met] = co_await ex_actor::WaitClusterState(
      /*predicate=*/[](const ex_actor::ClusterState& state) {
        return state.nodes.size() >= 2;
      },
      /*timeout_ms=*/5000);
  assert(condition_met);

  // 4. Pick a remote node, here we pick the first node whose address differs from ours.
  auto it = std::ranges::find_if(cluster_state.nodes, [&](const auto& n) { return n.address != listen_address; });
  auto remote_node_id = it->node_id;

  // 5. Create actor at remote node and play!
  auto ping_worker = co_await
      ex_actor::Spawn<&PingWorker::Create>(/*name=*/"Alice") // (2)
      .ToNode(remote_node_id);
  std::string ping_res = co_await ping_worker.Send<&PingWorker::Ping>("hello");
  assert(ping_res == "ack from Alice, msg got: hello");
  std::cout << "All work done" << std::endl;

  // 6. Wait for OS exit signal(like CTRL+C or kill) before shutting down, otherwise the process
  // will exit immediately, which might be earlier than the other node finishes its work,
  // causing error in the other node.
  co_await ex_actor::WaitOsExitSignal();
  ex_actor::Shutdown();
}

int main(int argc, char** argv) { stdexec::sync_wait(MainCoroutine(argc, argv)); }
```

1.  Such boilerplate is caused by the lack of reflection before C++26. We'll eliminate it in C++26, stay tuned!

2.  Note here in Spawn<>, you should provide the factory function pointer (the first argument of EXA_REMOTE) as the template argument. This differs from local actors, where you would pass the class type itself.


Compile this program into a binary, let's say `distributed_node`.

```bash
# usage: `./distributed_node <listen_address> [contact_address]`

# in one shell
./distributed_node tcp://127.0.0.1:5301

# in another shell
./distributed_node tcp://127.0.0.1:5302 tcp://127.0.0.1:5301
```

Both processes should print "All work done" log.

The process will block on `ex_actor::WaitOsExitSignal()`. You should kill them manually by CTRL+C or kill command.

## Fault tolerance

When a node can't be reached by any node in the cluster, it's considered dead, all in-flight remote calls will throw `ex_actor::NetworkError`, by catching this exception you can handle the failure gracefully. (`ex_actor::ConnectionLost` is a type alias kept for backward compatibility.)

```cpp
try {
  co_await ref.Send<&YourClass::Method>();
} catch (const ex_actor::NetworkError& e) {
  // e.remote_node_id tells you which node was lost
  // e.what() has a human-readable message
}
```

For example, now a node dies, you want to recreate your actor to another node:

```cpp
bool connection_lost = false;
try {
  co_await ref.Send<&YourClass::Method>();
} catch (const ex_actor::NetworkError& e) {
  connection_lost = true;
}

if (connection_lost) {
  std::cout << "I need a new node to join the cluster!" << std::endl;
  // 1. launch a new node, either by starting a process manually,
  // or using outer system's API if you are using k8s or slurm etc.

  // 2. wait for the new node to be ready.
  co_await ex_actor::WaitClusterState(...);

  // 3. recreate your actor to the new node. The original actor's state
  // is lost forever, it's your responsibility to handle the state recovery.
  auto new_ref = co_await ex_actor::Spawn<&YourClass::Create>().ToNode(new_node_id);
}
```

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