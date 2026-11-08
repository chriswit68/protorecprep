#include "prp/core/item_queue.h"

#include <utility>

namespace prp {
namespace {

std::shared_ptr<CaptureItem> make_item(ItemKind kind, uint32_t stream_id, Direction dir, int64_t ts_ns) {
  auto item = std::make_shared<CaptureItem>();
  item->kind = kind;
  item->stream_id = stream_id;
  item->dir = dir;
  item->ts_ns = ts_ns;
  return item;
}

}  // namespace

const char* close_reason_name(CloseReason reason) {
  switch (reason) {
    case CloseReason::PeerClosed: return "peer_closed";
    case CloseReason::LocalShutdown: return "local_shutdown";
    case CloseReason::ProtocolError: return "protocol_error";
    case CloseReason::IdleTimeout: return "idle_timeout";
    case CloseReason::ServerShutdown: return "server_shutdown";
    case CloseReason::Overflow: return "overflow";
    case CloseReason::AcceptRejected: return "accept_rejected";
    case CloseReason::SendError: return "send_error";
  }
  return "unknown";
}

ItemPtr make_stream_open(uint32_t stream_id, int64_t ts_ns, std::string peer, std::string type_name) {
  auto item = make_item(ItemKind::StreamOpen, stream_id, Direction::In, ts_ns);
  item->peer = std::move(peer);
  item->type_name = std::move(type_name);
  return item;
}

ItemPtr make_data(uint32_t stream_id, Direction dir, int64_t ts_ns, std::vector<uint8_t> bytes,
                  std::string type_name) {
  auto item = make_item(ItemKind::Data, stream_id, dir, ts_ns);
  item->data = std::move(bytes);
  item->type_name = std::move(type_name);
  return item;
}

ItemPtr make_stream_close(uint32_t stream_id, CloseReason reason, int64_t ts_ns) {
  auto item = make_item(ItemKind::StreamClose, stream_id, Direction::In, ts_ns);
  item->close_reason = static_cast<uint8_t>(reason);
  return item;
}

ItemPtr make_marker(int64_t ts_ns, std::string text) {
  auto item = make_item(ItemKind::Marker, 0, Direction::In, ts_ns);
  item->peer = std::move(text);
  return item;
}

BoundedItemQueue::BoundedItemQueue(size_t max_bytes, OverflowPolicy policy)
    : max_bytes_(max_bytes == 0 ? 1 : max_bytes), policy_(policy) {}

PushResult BoundedItemQueue::push(ItemPtr item) {
  if (!item) return PushResult::Closed;
  const size_t cost = item->accounted_bytes();

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

ItemPtr BoundedItemQueue::pop() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_data_.wait(lock, [&] { return closed_ || !queue_.empty(); });
  if (queue_.empty()) return nullptr;
  ItemPtr item = std::move(queue_.front());
  queue_.pop_front();
  if (bytes_ >= item->accounted_bytes()) {
    bytes_ -= item->accounted_bytes();
  } else {
    bytes_ = 0;
  }
  lock.unlock();
  cv_space_.notify_all();
  return item;
}

void BoundedItemQueue::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  cv_data_.notify_all();
  cv_space_.notify_all();
}

bool BoundedItemQueue::closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

void BoundedItemQueue::set_max_bytes(size_t max_bytes) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    max_bytes_ = max_bytes == 0 ? 1 : max_bytes;
  }
  cv_space_.notify_all();
}

size_t BoundedItemQueue::buffered_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bytes_;
}

size_t BoundedItemQueue::buffered_items() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

QueueStats BoundedItemQueue::stats() const {
  QueueStats s;
  s.pushed_items = pushed_items_.load(std::memory_order_relaxed);
  s.pushed_bytes = pushed_bytes_.load(std::memory_order_relaxed);
  s.dropped_items = dropped_items_.load(std::memory_order_relaxed);
  s.dropped_bytes = dropped_bytes_.load(std::memory_order_relaxed);
  s.max_buffered_bytes = max_buffered_bytes_.load(std::memory_order_relaxed);
  return s;
}

uint64_t BoundedItemQueue::dropped_bytes() const {
  return dropped_bytes_.load(std::memory_order_relaxed);
}
uint64_t BoundedItemQueue::dropped_items() const {
  return dropped_items_.load(std::memory_order_relaxed);
}

}  // namespace prp
