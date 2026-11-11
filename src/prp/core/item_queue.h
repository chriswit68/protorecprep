#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

#include "prp/core/capture_item.h"

namespace prp {

enum class OverflowPolicy : uint8_t {
  // Block the producer until the writer catches up. Applies TCP backpressure
  // to the client and guarantees that no captured byte is lost.
  Block = 0,
  // Discard the item and account for it. Recording continues.
  Drop = 1,
  // Discard the item and tell the session to close the connection.
  Disconnect = 2,
};

enum class PushResult : uint8_t { Ok = 0, Dropped = 1, Closed = 2, Overflow = 3 };

struct QueueStats {
  uint64_t pushed_items = 0;
  uint64_t pushed_bytes = 0;
  uint64_t dropped_items = 0;
  uint64_t dropped_bytes = 0;
  uint64_t max_buffered_bytes = 0;
};

// Queue payloads either are ItemPtr handles (shared_ptr) or small structs that
// expose the same information through members. The traits keep the queue
// independent of both payload types.
template <typename T>
struct QueueItemTraits {
  static bool empty(const T& item) { return item.empty(); }
  static size_t bytes(const T& item) { return item.accounted_bytes(); }
};

template <>
struct QueueItemTraits<ItemPtr> {
  static bool empty(const ItemPtr& item) { return !item; }
  static size_t bytes(const ItemPtr& item) { return item->accounted_bytes(); }
};

// Bounded multi-producer / single-consumer queue that accounts memory by the
// payload size of queued items.
template <typename T>
class BoundedQueue {
 public:
  BoundedQueue(size_t max_bytes, OverflowPolicy policy)
      : max_bytes_(max_bytes == 0 ? 1 : max_bytes), policy_(policy) {}

  // For Block policy this may wait until space is available. For Drop and
  // Disconnect it never blocks.
  PushResult push(T item) {
    if (QueueItemTraits<T>::empty(item)) return PushResult::Closed;
    const size_t cost = QueueItemTraits<T>::bytes(item);

    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) return PushResult::Closed;

    if (bytes_ + cost > max_bytes_) {
      switch (policy_) {
        case OverflowPolicy::Drop:
        case OverflowPolicy::Disconnect: {
          dropped_items_.fetch_add(1, std::memory_order_relaxed);
          dropped_bytes_.fetch_add(cost, std::memory_order_relaxed);
          return policy_ == OverflowPolicy::Drop ? PushResult::Dropped : PushResult::Overflow;
        }
        case OverflowPolicy::Block:
          // An item larger than the whole queue can only be admitted when the
          // queue is empty; waiting for `bytes_ + cost <= max_bytes_` would
          // never succeed and would deadlock the producer.
          if (cost > max_bytes_) {
            cv_space_.wait(lock, [&] { return closed_ || queue_.empty(); });
          } else {
            cv_space_.wait(lock, [&] { return closed_ || bytes_ + cost <= max_bytes_; });
          }
          if (closed_) return PushResult::Closed;
          break;
      }
    }

    bytes_ += cost;
    if (bytes_ > max_buffered_bytes_.load(std::memory_order_relaxed)) {
      max_buffered_bytes_.store(bytes_, std::memory_order_relaxed);
    }
    queue_.push_back(std::move(item));
    pushed_items_.fetch_add(1, std::memory_order_relaxed);
    pushed_bytes_.fetch_add(cost, std::memory_order_relaxed);
    lock.unlock();
    cv_data_.notify_one();
    return PushResult::Ok;
  }

  // Blocks until an item is available or the queue is closed. Returns a
  // default-constructed value when closed and drained.
  T pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_data_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return T();
    const size_t cost = QueueItemTraits<T>::bytes(queue_.front());
    T item = std::move(queue_.front());
    queue_.pop_front();
    if (bytes_ >= cost) {
      bytes_ -= cost;
    } else {
      bytes_ = 0;
    }
    lock.unlock();
    cv_space_.notify_all();
    return item;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    cv_data_.notify_all();
    cv_space_.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  void set_max_bytes(size_t max_bytes) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      max_bytes_ = max_bytes == 0 ? 1 : max_bytes;
    }
    cv_space_.notify_all();
  }

  size_t buffered_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
  }

  size_t buffered_items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  QueueStats stats() const {
    QueueStats s;
    s.pushed_items = pushed_items_.load(std::memory_order_relaxed);
    s.pushed_bytes = pushed_bytes_.load(std::memory_order_relaxed);
    s.dropped_items = dropped_items_.load(std::memory_order_relaxed);
    s.dropped_bytes = dropped_bytes_.load(std::memory_order_relaxed);
    s.max_buffered_bytes = max_buffered_bytes_.load(std::memory_order_relaxed);
    return s;
  }

  uint64_t dropped_bytes() const { return dropped_bytes_.load(std::memory_order_relaxed); }
  uint64_t dropped_items() const { return dropped_items_.load(std::memory_order_relaxed); }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_data_;
  std::condition_variable cv_space_;
  std::deque<T> queue_;
  size_t bytes_ = 0;
  size_t max_bytes_;
  OverflowPolicy policy_;
  bool closed_ = false;
  std::atomic<uint64_t> pushed_items_{0};
  std::atomic<uint64_t> pushed_bytes_{0};
  std::atomic<uint64_t> dropped_items_{0};
  std::atomic<uint64_t> dropped_bytes_{0};
  std::atomic<uint64_t> max_buffered_bytes_{0};
};

using BoundedItemQueue = BoundedQueue<ItemPtr>;

}  // namespace prp
