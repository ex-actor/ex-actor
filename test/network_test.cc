#include "ex_actor/internal/network.h"

#include <thread>

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include "ex_actor/api.h"

using ex_actor::internal::network::ByteBufferType;
namespace logging = ex_actor::internal::logging;

TEST(NetworkTest, MessageBrokerTest) {
  auto test_once = []() {
    std::vector<ex_actor::NodeInfo> node_list = {{.node_id = 0, .address = "tcp://127.0.0.1:5201"},
                                                 {.node_id = 1, .address = "tcp://127.0.0.1:5202"},
                                                 {.node_id = 2, .address = "tcp://127.0.0.1:5203"}};

    auto node_main = [](const std::vector<ex_actor::NodeInfo>& node_list, uint32_t node_id) {
      ex_actor::internal::util::SetThreadName("node_" + std::to_string(node_id));
      ex_actor::internal::network::MessageBroker message_broker(
          node_list,
          /*this_node_id=*/node_id, [&message_broker](uint64_t received_request_id, ByteBufferType data) {
            message_broker.ReplyRequest(received_request_id, std::move(data));
          });
      uint32_t to_node_id = (node_id + 1) % node_list.size();
      exec::async_scope scope;
      for (int i = 0; i < 5; ++i) {
        scope.spawn(message_broker.SendRequest(to_node_id, ByteBufferType(std::to_string(node_id))) |
                    stdexec::then([node_id](ByteBufferType data) {
                      std::string data_str(static_cast<char*>(data.data()), data.size());
                      logging::Info("got response data, node id: {}, data: {}", node_id, data_str);
                      ASSERT_EQ(data_str, std::to_string(node_id));
                    }));
      }
      stdexec::sync_wait(scope.on_empty());
    };
    std::jthread node_0(node_main, node_list, 0);
    std::jthread node_1(node_main, node_list, 1);
    std::jthread node_2(node_main, node_list, 2);
  };

  for (int i = 0; i < 10; ++i) {
    logging::Info("test once, {}", i);
    test_once();
  }
}
