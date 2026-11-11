#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "prp/core/item_queue.h"
#include "prp/record/format.h"
#include "prp/util/fs.h"

namespace prp {

struct RecorderConfig {
  fs::path out_dir;
  rec::Framing framing = rec::Framing::VarintLength;
  Sha256Digest proto_sha256{};
  std::string descriptor_set;
  std::string session_id;

  size_t max_queue_bytes = 256ull * 1024 * 1024;
  OverflowPolicy overflow = OverflowPolicy::Block;
  size_t write_batch_bytes = 1ull * 1024 * 1024;
  std::chrono::milliseconds fsync_interval{1000};
  bool durable = false;
  uint64_t rotate_bytes = 0;  // 0 = no size cap
  std::chrono::milliseconds rotate_interval{60 * 1000};  // capture chunk duration
};

struct RecorderStats {
  uint64_t records = 0;
  uint64_t data_bytes = 0;
  uint64_t segments = 0;
  uint64_t dropped_items = 0;
  uint64_t dropped_bytes = 0;
  uint64_t write_errors = 0;
  uint64_t buffered_bytes = 0;
  bool failed = false;
};

// Owns the capture file writer thread. All disk IO happens on that thread;
// producers only enqueue immutable items.
class Recorder {
 public:
  explicit Recorder(RecorderConfig config);
  ~Recorder();

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  bool start(std::string& err);
  void stop();

  BoundedItemQueue& queue() { return queue_; }
  PushResult submit(const ItemPtr& item);

  // Invoked from the recorder writer thread for every item that was appended to
  // the capture, together with the effective chunk/segment index. The JSON
  // sidecar uses this to mirror capture chunks exactly. The callback must not
  // block.
  using ItemObserver = std::function<void(const ItemPtr& item, uint32_t segment)>;
  void set_item_observer(ItemObserver observer);

  // Invoked once from the writer thread when an unrecoverable write error
  // occurs. Must be thread-safe and should only schedule shutdown.
  void set_fatal_callback(std::function<void(std::string)> callback);

  bool failed() const { return failed_.load(std::memory_order_relaxed); }
  RecorderStats stats() const;
  std::string last_error() const;
  const fs::path& session_dir() const { return config_.out_dir; }

 private:
  void run();
  bool open_segment(std::string& err);
  bool close_segment(bool clean, std::string& err);
  bool append_record(rec::RecordType type, const std::vector<uint8_t>& payload, std::string& err);
  bool append_record_unchecked(rec::RecordType type, const std::vector<uint8_t>& payload, std::string& err);
  bool flush_batch(std::string& err);
  bool maybe_rotate(size_t needed, std::string& err);
  bool ensure_synced(std::string& err, bool force);
  bool write_all(const uint8_t* data, size_t n, std::string& err);
  void fail(std::string error);
  void write_summary(bool clean);
  std::string segment_path(uint32_t index) const;

  RecorderConfig config_;
  BoundedItemQueue queue_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> started_{false};

  // Writer-thread only state.
  int fd_ = -1;
  uint32_t segment_index_ = 0;
  uint64_t segment_size_ = 0;
  uint64_t segment_records_ = 0;
  uint64_t segment_data_bytes_ = 0;
  std::chrono::steady_clock::time_point segment_start_{};
  std::chrono::steady_clock::time_point last_sync_{};
  std::vector<uint8_t> batch_;
  std::vector<uint8_t> payload_;
  std::map<uint32_t, std::pair<std::string, std::string>> active_streams_;
  int64_t session_start_ns_ = 0;

  std::atomic<uint64_t> total_records_{0};
  std::atomic<uint64_t> total_data_bytes_{0};
  std::atomic<uint64_t> total_segments_{0};
  std::atomic<uint64_t> write_errors_{0};

  mutable std::mutex error_mutex_;
  std::string error_;
  std::mutex callback_mutex_;
  std::function<void(std::string)> fatal_callback_;
  std::mutex observer_mutex_;
  ItemObserver observer_;
};

}  // namespace prp
