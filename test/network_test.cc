#include "ex_actor/internal/network.h"

#include <thread>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "ex_actor/api.h"

using ex_actor::internal::network::ByteBufferType;

class DummyMessageBroker : public ex_actor::internal::network::MessageBroker {
 public:
  DummyMessageBroker(std::vector<ex_actor::NodeInfo> node_list, uint32_t this_node_id)
      : ex_actor::internal::network::MessageBroker(std::move(node_list), this_node_id) {
    StartIOThreads();
  }

 protected:
  void HandleRequestFromOtherNode(uint64_t receive_request_id, ByteBufferType data) override {
    ReplyRequest(receive_request_id, std::move(data));
  }
};

TEST(NetworkTest, MessageBrokerTest) {
  auto test_once = []() {
    std::vector<ex_actor::NodeInfo> node_list = {{.node_id = 0, .address = "tcp://127.0.0.1:5201"},
                                                 {.node_id = 1, .address = "tcp://127.0.0.1:5202"},
                                                 {.node_id = 2, .address = "tcp://127.0.0.1:5203"}};

    ex_actor::util::Semaphore unfinished_nodes(node_list.size());
    auto node_main = [&unfinished_nodes](const std::vector<ex_actor::NodeInfo>& node_list, uint32_t node_id) {
      ex_actor::internal::util::SetThreadName("node_" + std::to_string(node_id));
      DummyMessageBroker message_broker(node_list,
                                        /*this_node_id=*/node_id);
      uint32_t to_node_id = (node_id + 1) % node_list.size();
      exec::async_scope scope;
      for (int i = 0; i < 5; ++i) {
        scope.spawn(message_broker.SendRequest(to_node_id, ByteBufferType(std::to_string(node_id))) |
                    stdexec::then([node_id](ByteBufferType data) {
                      std::string data_str(static_cast<char*>(data.data()), data.size());
                      spdlog::info("got response data, node id: {}, data: {}", node_id, data_str);
                      ASSERT_EQ(data_str, std::to_string(node_id));
                    }));
      }
      stdexec::sync_wait(scope.on_empty());
      unfinished_nodes.Acquire(1);
      ex_actor::ex::sync_wait(unfinished_nodes.OnDrained());
      spdlog::info("node {} finished", node_id);
    };
    std::jthread node_0(node_main, node_list, 0);
    std::jthread node_1(node_main, node_list, 1);
    std::jthread node_2(node_main, node_list, 2);
  };

  for (int i = 0; i < 10; ++i) {
    spdlog::info("test once, {}", i);
    test_once();
  }
}