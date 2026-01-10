/*
MIT License

Copyright (c) 2025 dakingffo

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#if defined(_MSC_VER) && _MSC_VER > 1000 || defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 3)
#pragma once
#endif

#ifndef DAKING_MPSC_QUEUE_HPP
#define DAKING_MPSC_QUEUE_HPP

#ifndef DAKING_NO_TSAN
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define DAKING_NO_TSAN __attribute__((no_sanitize("thread")))
#else
#define DAKING_NO_TSAN
#endif
#else
#define DAKING_NO_TSAN
#endif
#endif  // !DAKING_NO_TSAN

#ifndef DAKING_HAS_CXX20_OR_ABOVE
#if defined(_MSC_VER)
#define DAKING_HAS_CXX20_OR_ABOVE _MSVC_LANG >= 202002L
#else
#define DAKING_HAS_CXX20_OR_ABOVE __cplusplus >= 202002L
#endif
#endif  // !DAKING_HAS_CXX20_OR_ABOVE

#ifndef DAKING_ALWAYS_INLINE
#if defined(_MSC_VER)
#define DAKING_ALWAYS_INLINE [[msvc::forceinline]]
#else
#define DAKING_ALWAYS_INLINE [[gnu::always_inline]]
#endif
#endif  // !DAKING_ALWAYS_INLINE

#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace ex_actor::embedded_3rd::daking {
/*
             SC                MP
     [tail]->[]->[]->[]->[]->[head]

             SC                MP
     [tail]->[]->[]->[]->[]->[head]

             SC                MP
     [tail]->[]->[]->[]->[]->[head]

                  ...

             Although all alive MPSC_queue instances share a global pool of nodes to reduce memory allocation overhead,
     the consumer of each MPSC_queue could be different.

             All producers has a thread_local pool of nodes to reduce contention on the global pool,
             and the cost of getting nodes from the global pool is O(1) , no matter ThreadLocalCapacity is 256 or
   larger. This is because the global pool is organized as a stack of chunks, each chunk contains ThreadLocalCapacity
   nodes, when allocate nodes from the global pool, we always pop a chunk from the stack, this is a cheap pointer
   exchange operation. And the consumer thread will push back the chunk to the global pool when its thread_local pool is
   full.

             The chunk is freely combined of nodes, and the nodes in a chunk are not required to be contiguous in
   memory. To achieve this, every node has a next_chunk_ pointer , and all of the nodes in a chunk are linked together
   via next_ pointer, In MPMC_queue instance, wo focus on the next_ pointer, which is used to link the nodes in the
   queue. And in chunk_stack, we focus on the next_chunk_ pointer, which is used to link the nodes in a chunk.

             The page list is used to manage the memory of nodes allocated from global pool, when the last instance is
   destructed, all pages will be deleted automatically.

     Page:
            Blue                                Green                               Red
     [ThreadLocalCapacity * node] -> [2 * ThreadLocalCapacity * node] -> [4 * ThreadLocalCapacity * node] -> ... ->
   nullptr Color the nodes of contiguous memory with the same color for better illustration.

     GLOBAL:
             TOP
             [[B][B][B][R][G][R]]     consumers pop chunks from here and producers push chunks to here
              ↓
     [[R][R][G][R][R][G]]
              ↓
             [[R][G][B][G][G][R]]     It is obvious that the nodes in a chunk are not required to be contiguous in
   memory. ↓               Actually, they are freely combined of nodes,
                             ...              ABA problem exists when read next_chunk_ and compare stack top pointer, so
   we use tagged pointer to avoid it. nullptr
*/

namespace detail {
template <typename Queue>
struct MPSC_node {
  MPSC_node() { next_.store(nullptr, std::memory_order_release); }
  ~MPSC_node() { /* Don't call destructor of value_ here*/ }

  union {
    typename Queue::value_type value_;
    MPSC_node* next_chunk_;
  };
  std::atomic<MPSC_node*> next_;
};

template <typename Queue>
struct MPSC_page {
  using size_type = typename Queue::size_type;
  using Node = MPSC_node<Queue>;
  MPSC_page(Node* node, size_type count, MPSC_page* next) : node_(node), count_(count), next_(next) {}
  ~MPSC_page() = default;

  Node* node_;
  size_type count_;
  MPSC_page* next_;
};

template <typename Queue>
struct MPSC_chunk_stack {
  using size_type = typename Queue::size_type;
  using Node = MPSC_node<Queue>;

  struct Tagged_ptr {
    Node* node_ = nullptr;
    size_type tag_ = 0;
  };

  MPSC_chunk_stack() = default;
  ~MPSC_chunk_stack() = default;

  DAKING_ALWAYS_INLINE void Reset() noexcept { top.store(Tagged_ptr {nullptr, 0}); }

  DAKING_NO_TSAN void Push(Node* chunk) noexcept /* Pointer Swap */ {
    Tagged_ptr new_top {chunk, 0};
    Tagged_ptr old_top = top.load(std::memory_order_relaxed);
    // If TB read old_top, and TA pop the old_top then
    do {
      new_top.node_->next_chunk_ = old_top.node_;
      // then B will read a invalid value(which is regard as object address at TA)
      // but B will not pass CAS.
      // Actually, this is a data race, but CAS protect B form UB.
      new_top.tag_ = old_top.tag_ + 1;
    } while (!top.compare_exchange_weak(old_top, new_top, std::memory_order_acq_rel, std::memory_order_relaxed));
  }

  DAKING_NO_TSAN bool Try_pop(Node*& chunk) noexcept /* Pointer Swap */ {
    Tagged_ptr old_top = top.load(std::memory_order_acquire);
    Tagged_ptr new_top {};

    do {
      if (!old_top.node_) {
        return false;
      }
      new_top.node_ = old_top.node_->next_chunk_;
      new_top.tag_ = old_top.tag_ + 1;
      // If TA and TB reach here at the same time
      // And A pop the chunk successfully, then it will construct object at old_top.node_->next_chunk_,
      // so that B will read a invalid value, but this value will not pass the next CAS.(old_top have been updated by A)
      // Actually, this is a data race, but CAS protect B form UB.
    } while (!top.compare_exchange_weak(old_top, new_top, std::memory_order_acq_rel, std::memory_order_acquire));
    chunk = old_top.node_;
    return true;
  }

  std::atomic<Tagged_ptr> top {};
};

template <typename Queue>
struct MPSC_thread_hook {
  using size_type = typename Queue::size_type;
  using Manager = typename Queue::Manager;
  using Node = MPSC_node<Queue>;

  MPSC_thread_hook(std::thread::id tid, Node** node, size_type* size) : tid_(tid) {
    std::lock_guard<std::mutex> guard(Queue::global_mutex_);
    Manager* global_manager = Queue::global_manager_;
    // Only being called after global_manager is not a nullptr.
    global_manager->Register(tid_, node, size);
  }

  ~MPSC_thread_hook() {
    std::lock_guard<std::mutex> guard(Queue::global_mutex_);
    Manager* global_manager = Queue::global_manager_;
    if (global_manager) {
      global_manager->Unregister(tid_);
    } else {
      Queue::thread_local_node_list_ = nullptr;
      Queue::thread_local_node_count_ = 0;
    }
  }

  std::thread::id tid_;
};

template <typename Queue, typename Alloc>
struct MPSC_manager : public std::allocator_traits<Alloc>::template rebind_alloc<detail::MPSC_node<Queue>>,
                      public std::allocator_traits<Alloc>::template rebind_alloc<detail::MPSC_page<Queue>> {
  using size_type = typename Queue::size_type;
  using Node = MPSC_node<Queue>;
  using Page = MPSC_page<Queue>;
  using Thread_local_pair = std::pair<Node**, size_type*>;
  using Thread_local_manager =
      std::unordered_map<std::thread::id, Thread_local_pair, std::hash<std::thread::id>, std::equal_to<std::thread::id>,
                         typename std::allocator_traits<Alloc>::template rebind_alloc<
                             std::pair<const std::thread::id, Thread_local_pair>>>;
  using Alloc_node = typename std::allocator_traits<Alloc>::template rebind_alloc<Node>;
  using Altraits_node = std::allocator_traits<Alloc_node>;
  using Alloc_page = typename std::allocator_traits<Alloc>::template rebind_alloc<Page>;
  using Altraits_page = std::allocator_traits<Alloc_page>;

  MPSC_manager(const Alloc& alloc) : Alloc_node(alloc), Alloc_page(alloc) {}
  ~MPSC_manager() {
    for (auto& [tid, pair] : Get_global_thread_local_manager()) {
      auto [ptr, size] = pair;
      *ptr = nullptr;
      *size = 0;
    }
    while (global_page_list_) {
      Altraits_node::deallocate(*this, global_page_list_->node_, global_page_list_->count_);
      Altraits_page::deallocate(*this, std::exchange(global_page_list_, global_page_list_->next_), 1);
    }

    global_node_count_.store(0, std::memory_order_release);
  }

  void Reserve(size_type count) {
    Node* new_nodes = Altraits_node::allocate(*this, count);
    Page* new_page = Altraits_page::allocate(*this, 1);
    Altraits_page::construct(*this, new_page, new_nodes, count, global_page_list_);
    global_page_list_ = new_page;

    for (size_type i = 0; i < count; i++) {
      new_nodes[i].next_ = new_nodes + i + 1;
      if ((i & (Queue::thread_local_capacity - 1)) == Queue::thread_local_capacity - 1) [[unlikely]] {
        // chunk_count = count / ThreadLocalCapacity
        new_nodes[i].next_ = nullptr;
        std::atomic_thread_fence(std::memory_order_acq_rel);
        // mutex don't protect global_chunk_stack_, so we need make a atomice fence
        Queue::global_chunk_stack_.Push(&new_nodes[i - Queue::thread_local_capacity + 1]);
      }
    }

    global_node_count_.store(global_node_count_ + count, std::memory_order_release);
  }

  DAKING_ALWAYS_INLINE void Register(std::thread::id tid, Node** node, size_type* size) {
    // Already locked
    auto& global_thread_local_manager = Get_global_thread_local_manager();
    if (!global_thread_local_manager.count(tid)) {
      global_thread_local_manager[tid] = {node, size};
    }
  }

  DAKING_ALWAYS_INLINE void Unregister(std::thread::id tid) {
    // Already locked
    auto& global_thread_local_manager = Get_global_thread_local_manager();
    if (global_thread_local_manager.count(tid)) {
      global_thread_local_manager.erase(tid);
    }
  }

  DAKING_ALWAYS_INLINE size_type Node_count() noexcept { return global_node_count_.load(std::memory_order_acquire); }

  DAKING_ALWAYS_INLINE Thread_local_manager& Get_global_thread_local_manager() {
    static Thread_local_manager global_thread_local_manager(static_cast<const Alloc_node&>(*this));
    return global_thread_local_manager;
  }

  Page* global_page_list_ = nullptr;
  std::atomic<size_type> global_node_count_ = 0;
};
}  // namespace detail

template <typename Ty, std::size_t ThreadLocalCapacity = 256,
          std::size_t Align = 64, /* std::hardware_destructive_interference_size */
          typename Alloc = std::allocator<Ty>>
class MPSC_queue : /* CRTP for EBO */ private std::allocator_traits<Alloc>::template rebind_alloc<
                       detail::MPSC_manager<MPSC_queue<Ty, ThreadLocalCapacity, Align, Alloc>, Alloc>> {
 public:
  static_assert(std::is_object_v<Ty>, "Ty must be object.");
  static_assert((ThreadLocalCapacity & (ThreadLocalCapacity - 1)) == 0, "ThreadLocalCapacity must be a power of 2.");

  using value_type = Ty;
  using allocator_type = Alloc;
  using size_type = typename std::allocator_traits<allocator_type>::size_type;
  using pointer = typename std::allocator_traits<allocator_type>::pointer;
  using reference = Ty&;
  using const_reference = const Ty&;

  static constexpr std::size_t thread_local_capacity = ThreadLocalCapacity;
  static constexpr std::size_t align = Align;

 private:
  using Node = detail::MPSC_node<MPSC_queue>;
  using Page = detail::MPSC_page<MPSC_queue>;
  using Chunk_stack = detail::MPSC_chunk_stack<MPSC_queue>;
  using Hook = detail::MPSC_thread_hook<MPSC_queue>;
  using Manager = detail::MPSC_manager<MPSC_queue, Alloc>;
  using Alloc_manager = typename std::allocator_traits<allocator_type>::template rebind_alloc<Manager>;
  using Altraits_manager = std::allocator_traits<Alloc_manager>;
  using Alloc_node = typename Manager::Alloc_node;
  using Altraits_node = typename Manager::Altraits_node;
  using Alloc_page = typename Manager::Alloc_page;
  using Altraits_page = typename Manager::Altraits_page;

  static_assert(
      std::is_convertible_v<Alloc_manager, allocator_type> &&        // for get_allocator()
          std::is_constructible_v<Alloc_manager, allocator_type> &&  // for constructor of MPSC_queue
          std::is_constructible_v<Alloc_node, allocator_type> &&     // for constructor of MPSC_manager
          std::is_constructible_v<Alloc_page, allocator_type>,       // for constructor of MPSC_manager
      "Alloc should have a template constructor like 'Alloc(const Alloc<T>& alloc)' to meet internal conversion.");

  friend Hook;
  friend Manager;
  friend Altraits_node;
  friend Altraits_page;

 public:
  MPSC_queue(const allocator_type& alloc = allocator_type())
      : Alloc_manager(alloc) /* Alloc<Ty> -> Alloc<Manager>, which means Alloc should have a template constructor */ {
    global_instance_count_++;
    Try_create_global_manager(alloc);
    Initial();
  }

  explicit MPSC_queue(size_type initial_global_chunk_count, const allocator_type& alloc = allocator_type())
      : MPSC_queue(alloc) {
    reserve_global_chunk(initial_global_chunk_count);
  }

  ~MPSC_queue() {
    /* Warning: If queue is not empty, the value in left nodes will not be destructed! */
    if (--global_instance_count_ == 0) {
      // only the last instance free the global resource
      std::lock_guard<std::mutex> lock(global_mutex_);
      // if a new instance constructed before i get mutex, I do nothing.
      if (global_instance_count_ == 0) {
        Free_global();
      }
    }
  }

  MPSC_queue(const MPSC_queue&) = delete;
  MPSC_queue(MPSC_queue&&) = delete;
  MPSC_queue& operator=(const MPSC_queue&) = delete;
  MPSC_queue& operator=(MPSC_queue&&) = delete;

  template <typename... Args>
  DAKING_ALWAYS_INLINE void emplace(Args&&... args) {
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);
    Node* new_node = Allocate();
    Altraits_node::construct(Get_manager(), std::addressof(new_node->value_), std::forward<Args>(args)...);

    Node* old_head = head_.exchange(new_node, std::memory_order_acq_rel);
    old_head->next_.store(new_node, std::memory_order_release);
  }

  DAKING_ALWAYS_INLINE void enqueue(const_reference value) { emplace(value); }

  DAKING_ALWAYS_INLINE void enqueue(value_type&& value) { emplace(std::move(value)); }

  DAKING_ALWAYS_INLINE void enqueue_bulk(const_reference value, size_type n) {
    // N times thread_local operation, One time CAS operation.
    // So it is more efficient than N times enqueue.
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);

    Node* first_new_node = Allocate();
    Node* prev_node = first_new_node;
    Altraits_node::construct(Get_manager(), std::addressof(first_new_node->value_), value);
    for (size_type i = 1; i < n; i++) {
      Node* new_node = Allocate();
      Altraits_node::construct(Get_manager(), std::addressof(new_node->value_), value);
      prev_node->next_.store(new_node, std::memory_order_relaxed);
      prev_node = new_node;
    }
    Node* old_head = head_.exchange(prev_node, std::memory_order_acq_rel);
    old_head->next_.store(first_new_node, std::memory_order_release);
  }

  template <typename InputIt>
  DAKING_ALWAYS_INLINE void enqueue_bulk(InputIt it, size_type n) {
    // Enqueue n elements from input iterator.
    static_assert(std::is_base_of_v<std::input_iterator_tag, typename std::iterator_traits<InputIt>::iterator_category>,
                  "Iterator must be at least input iterator.");
    static_assert(std::is_same_v<typename std::iterator_traits<InputIt>::value_type, value_type>,
                  "The value type of iterator must be same as MPSC_queue::value_type.");
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);

    Node* first_new_node = Allocate();
    Node* prev_node = first_new_node;
    Altraits_node::construct(Get_manager(), std::addressof(first_new_node->value_), *it);
    ++it;
    for (size_type i = 1; i < n; i++) {
      Node* new_node = Allocate();
      Altraits_node::construct(Get_manager(), std::addressof(new_node->value_), *it);
      prev_node->next_.store(new_node, std::memory_order_relaxed);
      prev_node = new_node;
      ++it;
    }
    Node* old_head = head_.exchange(prev_node, std::memory_order_acq_rel);
    old_head->next_.store(first_new_node, std::memory_order_release);
  }

  template <typename ForwardIt,
            std::enable_if_t<std::is_base_of_v<std::forward_iterator_tag,
                                               typename std::iterator_traits<ForwardIt>::iterator_category>,
                             int> = 0>
  DAKING_ALWAYS_INLINE void enqueue_bulk(ForwardIt begin, ForwardIt end) {
    enqueue_bulk(begin, (size_type)std::distance(begin, end));
  }

  template <typename T>
  DAKING_ALWAYS_INLINE bool try_dequeue(T& value) noexcept(std::is_nothrow_assignable_v<T&, value_type&&> &&
                                                           std::is_nothrow_destructible_v<value_type>) {
    static_assert(std::is_assignable_v<T&, value_type&&>);
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);

    Node* next = tail_->next_.load(std::memory_order_acquire);
    if (next) [[likely]] {
      value = std::move(next->value_);
      Altraits_node::destroy(Get_manager(), std::addressof(next->value_));
      Deallocate(std::exchange(tail_, next));
      return true;
    } else {
      return false;
    }
  }

  template <typename OutputIt>
  DAKING_ALWAYS_INLINE size_type
  try_dequeue_bulk(OutputIt it, size_type n) noexcept(std::is_nothrow_assignable_v<decltype(*it), value_type&&> &&
                                                      std::is_nothrow_destructible_v<value_type> && noexcept(++it)) {
    static_assert(
        std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<OutputIt>::iterator_category> &&
                std::is_assignable_v<typename std::iterator_traits<OutputIt>::reference, value_type&&> ||
            std::is_same_v<typename std::iterator_traits<OutputIt>::iterator_category, std::output_iterator_tag>,
        "Iterator must be at least output iterator or forward iterator.");

    size_type count = 0;
    while (count < n && try_dequeue(*it)) {
      ++count;
      ++it;
    }
    return count;
  }

  template <typename ForwardIt,
            std::enable_if_t<std::is_base_of_v<std::forward_iterator_tag,
                                               typename std::iterator_traits<ForwardIt>::iterator_category>,
                             int> = 0>
  DAKING_ALWAYS_INLINE size_type try_dequeue_bulk(ForwardIt begin, ForwardIt end) noexcept(
      std::is_nothrow_assignable_v<decltype(*begin), value_type&&> && std::is_nothrow_destructible_v<value_type> &&
      noexcept(++begin)) {
    return try_dequeue_bulk(begin, (size_type)std::distance(begin, end));
  }

#if DAKING_HAS_CXX20_OR_ABOVE
  template <typename T>
  void dequeue(T& result) noexcept(std::is_nothrow_assignable_v<T&, value_type&&> &&
                                   std::is_nothrow_destructible_v<value_type>) {
    static_assert(std::is_assignable_v<T&, value_type&&>);

    while (true) {
      if (try_dequeue(result)) {
        return;
      }
      tail_->next_.wait(nullptr, std::memory_order_acquire);
    }
  }

  template <typename OutputIt>
  void dequeue_bulk(OutputIt it, size_type n) noexcept(std::is_nothrow_assignable_v<decltype(*it), value_type&&> &&
                                                       std::is_nothrow_destructible_v<value_type> && noexcept(++it)) {
    static_assert(
        std::is_base_of_v<std::forward_iterator_tag, typename std::iterator_traits<OutputIt>::iterator_category> &&
                std::is_assignable_v<typename std::iterator_traits<OutputIt>::reference, value_type> ||
            std::is_same_v<typename std::iterator_traits<OutputIt>::iterator_category, std::output_iterator_tag>,
        "Iterator must be at least output iterator or forward iterator.");

    size_type count = 0;
    while (count < n) {
      if (try_dequeue(*it)) {
        ++count;
        ++it;
      } else {
        tail_->next_.wait(nullptr, std::memory_order_acquire);
      }
    }
  }

  template <typename ForwardIt,
            std::enable_if_t<std::is_base_of_v<std::forward_iterator_tag,
                                               typename std::iterator_traits<ForwardIt>::iterator_category>,
                             int> = 0>
  DAKING_ALWAYS_INLINE void dequeue_bulk(ForwardIt begin, ForwardIt end) noexcept(
      std::is_nothrow_assignable_v<decltype(*begin), value_type&&> && std::is_nothrow_destructible_v<value_type> &&
      noexcept(++begin)) {
    dequeue_bulk(begin, (size_type)std::distance(begin, end));
  }
#endif

  DAKING_ALWAYS_INLINE bool empty() const noexcept { return tail_->next_.load(std::memory_order_acquire) == nullptr; }

  DAKING_ALWAYS_INLINE allocator_type get_allocator() noexcept { return allocator_type(*this); }

  DAKING_ALWAYS_INLINE static size_type global_node_size_apprx() noexcept {
    return global_manager_ ? global_manager_->Node_count() : 0;
  }

  DAKING_ALWAYS_INLINE static bool reserve_global_chunk(size_type chunk_count) {
    return global_manager_ ? Reserve_global_external(chunk_count) : false;
  }

 private:
  DAKING_ALWAYS_INLINE void Initial() {
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);

    Node* dummy = Allocate();
    tail_ = dummy;
    head_.store(dummy, std::memory_order_release);
  }

  DAKING_ALWAYS_INLINE Node* Allocate() {
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);
    if (!thread_local_node_list_) [[unlikely]] {
      while (!global_chunk_stack_.Try_pop(thread_local_node_list_)) {
        Reserve_global_internal();
      }
    }
    Node* res = std::exchange(thread_local_node_list_, thread_local_node_list_->next_);
    res->next_.store(nullptr, std::memory_order_relaxed);
    return res;
  }

  DAKING_ALWAYS_INLINE void Deallocate(Node* node) noexcept {
    static thread_local Hook thread_hook(std::this_thread::get_id(), &thread_local_node_list_,
                                         &thread_local_node_count_);

    node->next_ = thread_local_node_list_;
    thread_local_node_list_ = node;
    if (++thread_local_node_count_ >= thread_local_capacity) [[unlikely]] {
      global_chunk_stack_.Push(thread_local_node_list_);
      thread_local_node_list_ = nullptr;
      thread_local_node_count_ = 0;
    }
  }

  DAKING_ALWAYS_INLINE DAKING_NO_TSAN
      // If allocator is stateless, there is no data race.
      // But if it has stateful member: construct/destroy, you should protect these two functions by yourself,
      // and other functions are protected by daking.
      Manager&
      Get_manager() noexcept {
    return *global_manager_;
  }

  DAKING_ALWAYS_INLINE void Try_create_global_manager(const allocator_type& alloc) noexcept {
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (!global_manager_) {
      Manager* manager = Altraits_manager::allocate(*this, 1);
      Altraits_manager::construct(*this, manager, alloc);
      global_manager_ = manager;
    }
  }

  DAKING_ALWAYS_INLINE static bool Reserve_global_external(size_type chunk_count) {
    size_type global_node_count = global_manager_->Node_count();
    if (global_node_count / thread_local_capacity >= chunk_count) {
      return false;
    }
    std::lock_guard<std::mutex> lock(global_mutex_);
    global_node_count = global_manager_->Node_count();
    if (global_node_count / thread_local_capacity >= chunk_count) {
      return false;
    }

    size_type count = (chunk_count - global_node_count / thread_local_capacity) * thread_local_capacity;
    global_manager_->Reserve(count);
    return true;
  }

  DAKING_ALWAYS_INLINE static void Reserve_global_internal() {
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (global_chunk_stack_.top.load(std::memory_order_acquire).node_) {
      // if anyone have already allocate chunks, I return.
      return;
    }

    global_manager_->Reserve(std::max(thread_local_capacity, global_manager_->Node_count()));
  }

  DAKING_ALWAYS_INLINE void Free_global() {
    /* Already locked */
    global_chunk_stack_.Reset();
    Altraits_manager::destroy(*this, global_manager_);
    Altraits_manager::deallocate(*this, std::exchange(global_manager_, nullptr), 1);
  }

  /* Global LockFree*/
  inline static Chunk_stack global_chunk_stack_ {};
  inline static std::atomic<size_type> global_instance_count_ = 0;

  /* Global Mutex*/
  inline static std::mutex global_mutex_ {};
  inline static Manager* global_manager_ = nullptr;

  /* ThreadLocal*/
  inline thread_local static Node* thread_local_node_list_ = nullptr;
  inline thread_local static size_type thread_local_node_count_ = 0;

  /* MPSC */
  alignas(align) std::atomic<Node*> head_;
  alignas(align) Node* tail_;
};
}  // namespace ex_actor::embedded_3rd::daking

#endif  // !DAKING_MPSC_QUEUE_HPP