#include <chrono>
namespace ex_actor {
namespace internal {

constexpr size_t kEmptyActorRefHashVal = 10086;
constexpr auto kDefaultHeartbeatTimeout = std::chrono::milliseconds(2000);
constexpr auto kDefaultHeartbeatInterval = std::chrono::milliseconds(500);

}  // namespace internal
}  // namespace ex_actor
