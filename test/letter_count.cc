#include "ex_actor/api.h"

class DataLoader;
class Mapper;
class Reducer;

class DataLoader {
 public:
  static constexpr size_t kBlockSize = 1024 * 1024;  // 1MB
  explicit DataLoader(ex_actor::ActorRef<Mapper> mapper) : mapper_(mapper) {}
  exec::task<void> Run();
  exec::task<void> WaitFinished();

 private:
  size_t offset_ = 0;
  ex_actor::ActorRef<Mapper> mapper_;
  exec::async_scope async_scope_;
};

class Mapper {
 public:
  explicit Mapper(ex_actor::ActorRef<Reducer> reducer) : reducer_(reducer) {}
  void Map(const std::string& buf);
  exec::task<void> WaitFinished();

 private:
  ex_actor::ActorRef<Reducer> reducer_;
  exec::async_scope async_scope_;
};

class Reducer {
 public:
  void Reduce(std::unordered_map<char, int> partial);
  exec::task<void> WaitFinished();
  std::unordered_map<char, int> GetMergedCount() const { return merged_count_; }

 private:
  std::unordered_map<char, int> merged_count_;
};

// --------------implementation--------------

static exec::task<std::optional<std::string>> async_read(size_t offset, size_t size) {
  co_return std::optional<std::string>(std::string(size, 'a'));
}

exec::task<void> DataLoader::Run() {
  while (auto buffer = co_await async_read(offset_, kBlockSize)) {
    async_scope_.spawn(mapper_.Send<&Mapper::Map>(std::move(buffer.value())));
    offset_ += kBlockSize;
  }
}

void Mapper::Map(const std::string& buf) {
  std::unordered_map<char, int> count;
  for (char c : buf) count[c]++;
  async_scope_.spawn(reducer_.Send<&Reducer::Reduce>(std::move(count)));
}

void Reducer::Reduce(std::unordered_map<char, int> partial) {
  for (auto& [c, num] : partial) merged_count_[c] += num;
}

exec::task<void> DataLoader::WaitFinished() { co_await mapper_.Send<&Mapper::WaitFinished>(); }
exec::task<void> Mapper::WaitFinished() { co_await reducer_.Send<&Reducer::WaitFinished>(); }
exec::task<void> Reducer::WaitFinished() { co_return; }

static exec::task<void> MainCoroutine() {
  ex_actor::ActorRegistry registry(/*thread_pool_size=*/1);
  auto reducer = co_await registry.CreateActor<Reducer>();
  auto mapper = co_await registry.CreateActor<Mapper>(reducer);
  auto data_loader = co_await registry.CreateActor<DataLoader>(mapper);

  co_await data_loader.Send<&DataLoader::Run>();
  co_await data_loader.Send<&DataLoader::WaitFinished>();
  auto merged_count = co_await reducer.Send<&Reducer::GetMergedCount>();
  for (auto& [c, num] : merged_count) {
    std::cout << c << ": " << num << std::endl;
  }
}

int main() { stdexec::sync_wait(MainCoroutine()); }