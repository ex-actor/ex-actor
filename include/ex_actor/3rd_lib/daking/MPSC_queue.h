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

#ifndef EX_ACTOR_DAKING_MPSC_QUEUE_HPP
#define EX_ACTOR_DAKING_MPSC_QUEUE_HPP

#ifndef EX_ACTOR_DAKING_HAS_TSAN
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
extern "C" {
void AnnotateBenignRaceSized(const char* f, int l, const volatile void* mem, unsigned int size, const char* desc);
}
#define EX_ACTOR_DAKING_HAS_TSAN 1
#define EX_ACTOR_DAKING_NO_TSAN __attribute__((no_sanitize("thread")))
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_IGNORED(mem, size, desc) AnnotateBenignRaceSized(__FILE__, __LINE__, mem, size, desc)
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_ACQUIRE(mem) __tsan_acquire(mem)
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_RELEASE(mem) __tsan_release(mem)
#else
#define EX_ACTOR_DAKING_HAS_TSAN 0
#define EX_ACTOR_DAKING_NO_TSAN
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_IGNORED(mem, size, desc)
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_ACQUIRE(mem)
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_RELEASE(mem)
#endif
#else
#define EX_ACTOR_DAKING_HAS_TSAN 0
#define EX_ACTOR_DAKING_NO_TSAN
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_IGNORED(mem, size, desc)
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_ACQUIRE(mem)
#define EX_ACTOR_DAKING_TSAN_ANNOTATE_RELEASE(mem)
#endif
#endif  // !EX_ACTOR_DAKING_HAS_TSAN

#ifndef EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
#if defined(_MSC_VER)
#define EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE _MSVC_LANG >= 202002L
#else
#define EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE __cplusplus >= 202002L
#endif
#endif  // !EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE

#ifndef EX_ACTOR_DAKING_ALWAYS_INLINE
#if defined(_MSC_VER)
#define EX_ACTOR_DAKING_ALWAYS_INLINE [[msvc::forceinline]]
#else
#define EX_ACTOR_DAKING_ALWAYS_INLINE [[gnu::always_inline]]
#endif
#endif  // !EX_ACTOR_DAKING_ALWAYS_INLINE

#ifndef EX_ACTOR_DAKING_LIKELY
#if EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
#define EX_ACTOR_DAKING_LIKELY [[likely]]
#else
#define EX_ACTOR_DAKING_LIKELY
#endif
#endif  // !EX_ACTOR_DAKING_LIKELY

#ifndef EX_ACTOR_DAKING_UNLIKELY
#if EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
#define EX_ACTOR_DAKING_UNLIKELY [[unlikely]]
#else
#define EX_ACTOR_DAKING_UNLIKELY
#endif
#endif  // !EX_ACTOR_DAKING_UNLIKELY

#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

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
  using value_type = typename Queue::value_type;

  using node_t = MPSC_node;

  MPSC_node() { next_.store(nullptr, std::memory_order_release); }
  ~MPSC_node() { /* Don't call destructor of value_ here*/ }

  union {
    value_type value_;
    node_t* next_chunk_;
  };
  std::atomic<node_t*> next_;
};

template <typename Queue>
struct MPSC_page {
  using size_type = typename Queue::size_type;

  using node_t = MPSC_node<Queue>;
  using page_t = MPSC_page;

  MPSC_page(node_t* node, size_type count, page_t* next) : node_(node), count_(count), next_(next) {}
  ~MPSC_page() = default;

  size_type count_;
  node_t* node_;
  page_t* next_;
};

template <typename Queue>
struct MPSC_chunk_stack {
  using size_type = typename Queue::size_type;

  using node_t = MPSC_node<Queue>;

  struct tagged_ptr {
    node_t* node_ = nullptr;
    size_type tag_ = 0;
  };

  MPSC_chunk_stack() = default;
  ~MPSC_chunk_stack() = default;

  EX_ACTOR_DAKING_ALWAYS_INLINE void reset() noexcept { top_.store(tagged_ptr {nullptr, 0}); }

  EX_ACTOR_DAKING_ALWAYS_INLINE void push(node_t* chunk) noexcept /* Pointer Swap */ {
    tagged_ptr new_top {chunk, 0};
    tagged_ptr old_top = top_.load(std::memory_order_relaxed);
    // If TB read old_top, and TA pop the old_top then
    do {
      new_top.node_->next_chunk_ = old_top.node_;
      // then B will read a invalid value
      // but B will not pass CAS.
      // Actually, this is a data race, but CAS protect B form UB.
      new_top.tag_ = old_top.tag_ + 1;
    } while (!top_.compare_exchange_weak(old_top, new_top, std::memory_order_acq_rel, std::memory_order_relaxed));
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE bool try_pop(node_t*& chunk) noexcept /* Pointer Swap */ {
    tagged_ptr old_top = top_.load(std::memory_order_acquire);
    tagged_ptr new_top {};

    do {
      if (!old_top.node_) {
        return false;
      }
      EX_ACTOR_DAKING_TSAN_ANNOTATE_IGNORED(&old_top.node_->next_chunk_, sizeof(node_t*), "Reason: healthy data race");
      new_top.node_ = old_top.node_->next_chunk_;
      new_top.tag_ = old_top.tag_ + 1;
      // If TA and TB reach here at the same time
      // And A pop the chunk successfully, then it will construct object at old_top.node_->next_chunk_,
      // so that B will read a invalid value, but this value will not pass the next CAS.(old_top have been updated by A)
      // Actually, this is a data race, but CAS protect B form UB.
    } while (!top_.compare_exchange_weak(old_top, new_top, std::memory_order_acq_rel, std::memory_order_acquire));

    chunk = old_top.node_;

    return true;
  }

  std::atomic<tagged_ptr> top_ {};
};

template <typename Queue>
struct MPSC_thread_hook {
  using size_type = typename Queue::size_type;

  using node_t = MPSC_node<Queue>;
  using thread_local_t = typename Queue::thread_local_t;

  MPSC_thread_hook() : tid_(std::this_thread::get_id()) {
    std::lock_guard<std::mutex> guard(Queue::global_mutex_);
    // Only being called after global_manager is not a nullptr.
    pair_ = Queue::_get_global_manager().register_for(tid_);
  }

  ~MPSC_thread_hook() {
    // If this is consumer hook, release the queue tail to help destructor thread.
    std::atomic_thread_fence(std::memory_order_release);
    if (Queue::_is_global_manager_alive()) {
      std::lock_guard<std::mutex> guard(Queue::global_mutex_);
      Queue::_get_global_manager().unregister_for(tid_);
    }
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE node_t*& node_list() noexcept { return pair_->first; }

  EX_ACTOR_DAKING_ALWAYS_INLINE size_type& node_size() noexcept { return pair_->second; }

  std::thread::id tid_;
  thread_local_t* pair_;
};

// If allocator is stateless, there is no data race.
// But if it has stateful member: construct/destroy, you should protect these two functions by yourself,
// and other functions are protected by daking.
template <typename Queue, typename ThreadLocalType, typename Alloc>
struct MPSC_manager : public std::allocator_traits<Alloc>::template rebind_alloc<detail::MPSC_node<Queue>>,
                      public std::allocator_traits<Alloc>::template rebind_alloc<detail::MPSC_page<Queue>> {
  using size_type = typename Queue::size_type;

  using node_t = MPSC_node<Queue>;
  using page_t = MPSC_page<Queue>;
  using thread_local_t = ThreadLocalType;
  using thread_local_manager_t = std::unordered_map<std::thread::id, std::unique_ptr<thread_local_t>>;
  using thread_local_recycler_t = std::vector<std::unique_ptr<thread_local_t>>;
  using alloc_node_t = typename std::allocator_traits<Alloc>::template rebind_alloc<node_t>;
  using altraits_node_t = std::allocator_traits<alloc_node_t>;
  using alloc_page_t = typename std::allocator_traits<Alloc>::template rebind_alloc<page_t>;
  using altraits_page_t = std::allocator_traits<alloc_page_t>;

  MPSC_manager(const Alloc& alloc) : alloc_node_t(alloc), alloc_page_t(alloc) {}

  ~MPSC_manager() {
    reset();
    Queue::global_manager_instance_ = nullptr;
    std::atomic_thread_fence(std::memory_order_release);
  }

  void reset() {
    /* Already locked */
    for (auto& [tid, pair_ptr] : global_thread_local_manager_) {
      auto& [node, size] = *pair_ptr;
      node = nullptr;
      size = 0;
    }
    for (auto& pair_ptr : global_thread_local_recycler_) {
      auto& [node, size] = *pair_ptr;
      node = nullptr;
      size = 0;
    }

    while (global_page_list_) {
      altraits_node_t::deallocate(*this, global_page_list_->node_, global_page_list_->count_);
      altraits_page_t::deallocate(*this, std::exchange(global_page_list_, global_page_list_->next_), 1);
    }

    global_node_count_.store(0, std::memory_order_release);
  }

  void reserve(size_type count) {
    /* Already locked */
    node_t* new_nodes = altraits_node_t::allocate(*this, count);
    page_t* new_page = altraits_page_t::allocate(*this, 1);
    altraits_page_t::construct(*this, new_page, new_nodes, count, global_page_list_);
    global_page_list_ = new_page;

    for (size_type i = 0; i < count; i++) {
      new_nodes[i].next_ = new_nodes + i + 1;  // seq_cst
      if ((i & (Queue::thread_local_capacity - 1)) == Queue::thread_local_capacity - 1) EX_ACTOR_DAKING_UNLIKELY {
          // chunk_count = count / ThreadLocalCapacity
          new_nodes[i].next_ = nullptr;
          std::atomic_thread_fence(std::memory_order_acq_rel);
          // mutex don't protect global_chunk_stack_
          Queue::global_chunk_stack_.push(&new_nodes[i - Queue::thread_local_capacity + 1]);
        }
    }

    global_node_count_.store(global_node_count_ + count, std::memory_order_release);
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE thread_local_t* register_for(std::thread::id tid) {
    /* Already locked */
    if (!global_thread_local_recycler_.empty()) {
      global_thread_local_manager_[tid] = std::move(global_thread_local_recycler_.back());
      global_thread_local_recycler_.pop_back();
    } else {
      global_thread_local_manager_[tid] = std::make_unique<thread_local_t>(nullptr, 0);
    }
    return global_thread_local_manager_[tid].get();
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE void unregister_for(std::thread::id tid) {
    /* Already locked */
    global_thread_local_recycler_.push_back(std::move(global_thread_local_manager_[tid]));
    global_thread_local_manager_.erase(tid);
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE size_type node_count() noexcept { return global_node_count_.load(std::memory_order_acquire); }

  EX_ACTOR_DAKING_ALWAYS_INLINE static MPSC_manager* create_global_manager(const Alloc& alloc) {
    static MPSC_manager global_manager(alloc);
    return &global_manager;
  }

  page_t* global_page_list_ = nullptr;
  std::atomic<size_type> global_node_count_ = 0;
  thread_local_manager_t global_thread_local_manager_;
  thread_local_recycler_t global_thread_local_recycler_;
};
}  // namespace detail

template <typename Ty, std::size_t ThreadLocalCapacity = 256,
          std::size_t Align = 64, /* std::hardware_destructive_interference_size */
          typename Alloc = std::allocator<Ty>>
class MPSC_queue {
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
  using node_t = detail::MPSC_node<MPSC_queue>;
  using page_t = detail::MPSC_page<MPSC_queue>;
  using chunk_stack_t = detail::MPSC_chunk_stack<MPSC_queue>;
  using thread_hook_t = detail::MPSC_thread_hook<MPSC_queue>;
  using thread_local_t = std::pair<node_t*, size_type>;
  using manager_t = detail::MPSC_manager<MPSC_queue, thread_local_t, allocator_type>;
  using alloc_node_t = typename manager_t::alloc_node_t;
  using altraits_node_t = typename manager_t::altraits_node_t;
  using alloc_page_t = typename manager_t::alloc_page_t;
  using altraits_page_t = typename manager_t::altraits_page_t;

  static_assert(std::is_empty_v<allocator_type>,
                "In the global manager design, Alloc must be stateless to avoid dangling references. ");

  static_assert(
      std::is_constructible_v<alloc_node_t, allocator_type> && std::is_constructible_v<alloc_page_t, allocator_type>,
      "Alloc should have a template constructor like 'Alloc(const Alloc<T>& alloc)' to meet internal conversion.");

  friend thread_hook_t;
  friend manager_t;
  friend altraits_node_t;
  friend altraits_page_t;

 public:
  MPSC_queue() : MPSC_queue(allocator_type()) {}

  MPSC_queue(const allocator_type& alloc) {
    /* Alloc<Ty> -> Alloc<...>, which means Alloc should have a template constructor */
    global_instance_count_++;
    _initial(alloc);
  }

  explicit MPSC_queue(size_type initial_global_chunk_count, const allocator_type& alloc = allocator_type())
      : MPSC_queue(alloc) {
    reserve_global_chunk(initial_global_chunk_count);
  }

  ~MPSC_queue() {
    node_t* next = tail_->next_.load(std::memory_order_acquire);
    while (next) {
      altraits_node_t::destroy(_get_global_manager(), std::addressof(next->value_));
      _deallocate(std::exchange(tail_, next));
      next = tail_->next_.load(std::memory_order_acquire);
    }
    _deallocate(tail_);

    if (--global_instance_count_ == 0) {
      // only the last instance free the global resource
      std::lock_guard<std::mutex> lock(global_mutex_);
      // if a new instance constructed before i get mutex, I do nothing.
      if (global_instance_count_ == 0) {
        _free_global();
      }
    }
  }

  MPSC_queue(const MPSC_queue&) = delete;
  MPSC_queue(MPSC_queue&&) = delete;
  MPSC_queue& operator=(const MPSC_queue&) = delete;
  MPSC_queue& operator=(MPSC_queue&&) = delete;

  template <typename... Args>
  EX_ACTOR_DAKING_ALWAYS_INLINE void emplace(Args&&... args) {
    node_t* new_node = _allocate();
    altraits_node_t::construct(_get_global_manager(), std::addressof(new_node->value_), std::forward<Args>(args)...);

    node_t* old_head = head_.exchange(new_node, std::memory_order_acq_rel);
    old_head->next_.store(new_node, std::memory_order_release);
#if EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
    old_head->next_.notify_one();
#endif
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE void enqueue(const_reference value) { emplace(value); }

  EX_ACTOR_DAKING_ALWAYS_INLINE void enqueue(value_type&& value) { emplace(std::move(value)); }

  EX_ACTOR_DAKING_ALWAYS_INLINE void enqueue_bulk(const_reference value, size_type n) {
    // N times thread_local operation, One time CAS operation.
    // So it is more efficient than N times enqueue.
    if (n == 0) EX_ACTOR_DAKING_UNLIKELY {
        return;
      }

    node_t* first_new_node = _allocate();
    node_t* prev_node = first_new_node;
    altraits_node_t::construct(_get_global_manager(), std::addressof(first_new_node->value_), value);
    for (size_type i = 1; i < n; i++) {
      node_t* new_node = _allocate();
      altraits_node_t::construct(_get_global_manager(), std::addressof(new_node->value_), value);
      prev_node->next_.store(new_node, std::memory_order_relaxed);
      prev_node = new_node;
    }
    node_t* old_head = head_.exchange(prev_node, std::memory_order_acq_rel);
    old_head->next_.store(first_new_node, std::memory_order_release);
#if EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
    old_head->next_.notify_one();
#endif
  }

  template <typename InputIt>
  EX_ACTOR_DAKING_ALWAYS_INLINE void enqueue_bulk(InputIt it, size_type n) {
    // Enqueue n elements from input iterator.
    static_assert(std::is_base_of_v<std::input_iterator_tag, typename std::iterator_traits<InputIt>::iterator_category>,
                  "Iterator must be at least input iterator.");
    static_assert(std::is_same_v<typename std::iterator_traits<InputIt>::value_type, value_type>,
                  "The value type of iterator must be same as MPSC_queue::value_type.");
    if (n == 0) EX_ACTOR_DAKING_UNLIKELY {
        return;
      }

    node_t* first_new_node = _allocate();
    node_t* prev_node = first_new_node;
    altraits_node_t::construct(_get_global_manager(), std::addressof(first_new_node->value_), *it);
    ++it;
    for (size_type i = 1; i < n; i++) {
      node_t* new_node = _allocate();
      altraits_node_t::construct(_get_global_manager(), std::addressof(new_node->value_), *it);
      prev_node->next_.store(new_node, std::memory_order_relaxed);
      prev_node = new_node;
      ++it;
    }
    node_t* old_head = head_.exchange(prev_node, std::memory_order_acq_rel);
    old_head->next_.store(first_new_node, std::memory_order_release);
#if EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
    old_head->next_.notify_one();
#endif
  }

  template <typename ForwardIt,
            std::enable_if_t<std::is_base_of_v<std::forward_iterator_tag,
                                               typename std::iterator_traits<ForwardIt>::iterator_category>,
                             int> = 0>
  EX_ACTOR_DAKING_ALWAYS_INLINE void enqueue_bulk(ForwardIt begin, ForwardIt end) {
    enqueue_bulk(begin, (size_type)std::distance(begin, end));
  }

  template <typename T>
  EX_ACTOR_DAKING_ALWAYS_INLINE bool try_dequeue(T& value) noexcept(std::is_nothrow_assignable_v<T&, value_type&&> &&
                                                           std::is_nothrow_destructible_v<value_type>) {
    static_assert(std::is_assignable_v<T&, value_type&&>);

    node_t* next = tail_->next_.load(std::memory_order_acquire);
    if (next) EX_ACTOR_DAKING_LIKELY {
        value = std::move(next->value_);
        altraits_node_t::destroy(_get_global_manager(), std::addressof(next->value_));
        _deallocate(std::exchange(tail_, next));
        return true;
      }
    else {
      return false;
    }
  }

  template <typename OutputIt>
  EX_ACTOR_DAKING_ALWAYS_INLINE size_type
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
  EX_ACTOR_DAKING_ALWAYS_INLINE size_type try_dequeue_bulk(ForwardIt begin, ForwardIt end) noexcept(
      std::is_nothrow_assignable_v<decltype(*begin), value_type&&> && std::is_nothrow_destructible_v<value_type> &&
      noexcept(++begin)) {
    return try_dequeue_bulk(begin, (size_type)std::distance(begin, end));
  }

#if EX_ACTOR_DAKING_HAS_CXX20_OR_ABOVE
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
  EX_ACTOR_DAKING_ALWAYS_INLINE void dequeue_bulk(ForwardIt begin, ForwardIt end) noexcept(
      std::is_nothrow_assignable_v<decltype(*begin), value_type&&> && std::is_nothrow_destructible_v<value_type> &&
      noexcept(++begin)) {
    dequeue_bulk(begin, (size_type)std::distance(begin, end));
  }
#endif

  EX_ACTOR_DAKING_ALWAYS_INLINE bool empty() const noexcept { return tail_->next_.load(std::memory_order_acquire) == nullptr; }

  EX_ACTOR_DAKING_ALWAYS_INLINE static size_type global_node_size_apprx() noexcept {
    return _is_global_manager_alive() ? _get_global_manager().node_count() : 0;
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE static bool reserve_global_chunk(size_type chunk_count) {
    return _is_global_manager_alive() ? _reserve_global_external(chunk_count) : false;
  }

 private:
  EX_ACTOR_DAKING_ALWAYS_INLINE static manager_t& _get_global_manager() noexcept { return *global_manager_instance_; }

  EX_ACTOR_DAKING_ALWAYS_INLINE static bool _is_global_manager_alive() noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    return global_manager_instance_ != nullptr;
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE thread_hook_t& _get_thread_hook() {
    static thread_local thread_hook_t thread_hook;
    return thread_hook;
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE node_t*& _get_thread_local_node_list() noexcept { return _get_thread_hook().node_list(); }

  EX_ACTOR_DAKING_ALWAYS_INLINE size_type& _get_thread_local_node_size() noexcept { return _get_thread_hook().node_size(); }

  EX_ACTOR_DAKING_ALWAYS_INLINE void _initial(const Alloc& alloc) {
    {
      std::lock_guard<std::mutex> guard(global_mutex_);
      global_manager_instance_ = manager_t::create_global_manager(alloc);  // single instance
    }

    node_t* dummy = _allocate();
    tail_ = dummy;
    head_.store(dummy, std::memory_order_release);
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE node_t* _allocate() {
    node_t*& thread_local_node_list = _get_thread_local_node_list();
    size_type& thread_local_node_size = _get_thread_local_node_size();
    if (thread_local_node_size == 0) EX_ACTOR_DAKING_UNLIKELY {
        while (!global_chunk_stack_.try_pop(thread_local_node_list)) {
          _reserve_global_internal();
        }
        thread_local_node_size = thread_local_capacity;
      }
    thread_local_node_size--;
    EX_ACTOR_DAKING_TSAN_ANNOTATE_ACQUIRE(thread_local_node_list);
    EX_ACTOR_DAKING_TSAN_ANNOTATE_ACQUIRE(thread_local_node_list->next_);
    node_t* res = std::exchange(thread_local_node_list, thread_local_node_list->next_.load(std::memory_order_relaxed));
    res->next_.store(nullptr, std::memory_order_relaxed);
    return res;
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE void _deallocate(node_t* node) noexcept {
    node_t*& thread_local_node_list = _get_thread_local_node_list();
    node->next_.store(thread_local_node_list, std::memory_order_relaxed);
    thread_local_node_list = node;
    EX_ACTOR_DAKING_TSAN_ANNOTATE_RELEASE(node);
    if (++_get_thread_local_node_size() >= thread_local_capacity) EX_ACTOR_DAKING_UNLIKELY {
        global_chunk_stack_.push(thread_local_node_list);
        thread_local_node_list = nullptr;
        _get_thread_local_node_size() = 0;
      }
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE static bool _reserve_global_external(size_type chunk_count) {
    manager_t& manager = _get_global_manager();
    size_type global_node_count = manager.node_count();
    if (global_node_count / thread_local_capacity >= chunk_count) {
      return false;
    }
    std::lock_guard<std::mutex> lock(global_mutex_);
    global_node_count = manager.node_count();
    if (global_node_count / thread_local_capacity >= chunk_count) {
      return false;
    }

    size_type count = (chunk_count - global_node_count / thread_local_capacity) * thread_local_capacity;
    manager.reserve(count);
    return true;
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE static void _reserve_global_internal() {
    std::lock_guard<std::mutex> lock(global_mutex_);
    if (global_chunk_stack_.top_.load(std::memory_order_acquire).node_) {
      // if anyone have already allocate chunks, I return.
      return;
    }

    _get_global_manager().reserve(std::max(thread_local_capacity, _get_global_manager().node_count()));
  }

  EX_ACTOR_DAKING_ALWAYS_INLINE void _free_global() {
    /* Already locked */
    global_chunk_stack_.reset();
    if (_is_global_manager_alive()) {
      _get_global_manager().reset();
    }
  }

  /* Global LockFree*/
  inline static chunk_stack_t global_chunk_stack_ {};
  inline static std::atomic<size_type> global_instance_count_ = 0;

  /* Global Mutex*/
  inline static std::mutex global_mutex_ {};
  inline static manager_t* global_manager_instance_ = nullptr;

  /* MPSC */
  alignas(align) std::atomic<node_t*> head_;
  alignas(align) node_t* tail_;
};
}  // namespace ex_actor::embedded_3rd::daking

#endif  // !EX_ACTOR_DAKING_MPSC_QUEUE_HPP