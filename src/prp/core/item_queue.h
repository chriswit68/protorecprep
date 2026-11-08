#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

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

// Bounded multi-producer / single-consumer queue that accounts memory by the
// payload size of queued items.
class BoundedItemQueue {
 public:
  BoundedItemQueue(size_t max_bytes, OverflowPolicy policy);

  // For Block policy this may wait until space is available. For Drop and
  // Disconnect it never blocks.
  PushResult push(ItemPtr item);

  // Blocks until an item is available or the queue is closed. Returns nullptr
  // when closed and drained.
  ItemPtr pop();

  void close();
  bool closed() const;

  void set_max_bytes(size_t max_bytes);
  size_t buffered_bytes() const;
  size_t buffered_items() const;

  QueueStats stats() const;
  uint64_t dropped_bytes() const;
  uint64_t dropped_items() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_data_;
  std::condition_variable cv_space_;
  std::deque<ItemPtr> queue_;
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

}  // namespace prp
