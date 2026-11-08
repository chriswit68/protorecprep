#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "prp/core/item_queue.h"
#include "prp/net/framer.h"
#include "prp/proto/schema.h"
#include "prp/record/format.h"
#include "prp/util/fs.h"

namespace prp {

struct JsonWriterConfig {
  fs::path output_path;  // events.jsonl
  const Schema* schema = nullptr;
  std::string default_type_name;
  rec::Framing framing = rec::Framing::VarintLength;
  size_t max_queue_bytes = 64ull * 1024 * 1024;
  size_t max_frame = 16ull * 1024 * 1024;
  bool print_defaults = false;
  bool preserve_proto_names = false;
};

struct JsonWriterStats {
  uint64_t events = 0;
  uint64_t messages = 0;
  uint64_t unparsed = 0;
  uint64_t parse_errors = 0;
  uint64_t dropped_items = 0;
  uint64_t dropped_bytes = 0;
  uint64_t desynced_streams = 0;
  uint64_t skipped_data_items = 0;
  uint64_t bytes_written = 0;
  bool failed = false;
};

// Converts captured byte chunks into JSONL asynchronously. It never blocks the
// capture path: if the queue is full, events are dropped and accounted for.
class JsonWriter {
 public:
  explicit JsonWriter(JsonWriterConfig config);
  ~JsonWriter();

  JsonWriter(const JsonWriter&) = delete;
  JsonWriter& operator=(const JsonWriter&) = delete;

  bool start(std::string& err);
  void stop();

  BoundedItemQueue& queue() { return queue_; }

  // Never blocks. If the queue is full, the item is dropped and the stream is
  // marked desynchronized so the worker stops attempting to reassemble frames
  // from a hole in the byte stream.
  PushResult submit(const ItemPtr& item) {
    const PushResult result = queue_.push(item);
    if (result == PushResult::Dropped && item) {
      std::lock_guard<std::mutex> lock(drop_mutex_);
      desynced_.insert(item->stream_id);
    }
    return result;
  }

  JsonWriterStats stats() const;
  std::string last_error() const;

 private:
  struct StreamState;
  void run();
  void flush_buffer(bool force);
  void emit_event_prefix(const char* event, uint32_t stream_id, int64_t ts_ns);
  void emit_message(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view type_name,
                    const google::protobuf::Message& message);
  void emit_unparsed(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view reason,
                     const uint8_t* bytes, size_t len);
  void handle_open(const CaptureItem& item);
  void handle_data(const CaptureItem& item);
  void handle_close(const CaptureItem& item);
  void handle_marker(const CaptureItem& item);
  StreamState* state_for(uint32_t stream_id, bool create);
  void prepare_channel(StreamState& state, uint32_t stream_id, bool outbound,
                       const std::string& type_name);
  bool take_desynced(uint32_t stream_id);

  JsonWriterConfig config_;
  BoundedItemQueue queue_;
  std::thread thread_;
  FILE* file_ = nullptr;
  std::string buffer_;
  std::atomic<uint64_t> events_{0};
  std::atomic<uint64_t> messages_{0};
  std::atomic<uint64_t> unparsed_{0};
  std::atomic<uint64_t> parse_errors_{0};
  std::atomic<uint64_t> desynced_streams_{0};
  std::atomic<uint64_t> skipped_data_items_{0};
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<bool> failed_{false};

  std::mutex drop_mutex_;
  std::unordered_set<uint32_t> desynced_;

  mutable std::mutex error_mutex_;
  std::string error_;
  std::atomic<bool> started_{false};

  // Worker-thread state.
  std::unique_ptr<google::protobuf::DynamicMessageFactory> factory_;
  std::vector<std::unique_ptr<StreamState>> streams_;
  uint64_t seq_ = 0;
  std::chrono::steady_clock::time_point last_flush_{};
};

}  // namespace prp
