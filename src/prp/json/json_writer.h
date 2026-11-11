#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
  fs::path output_path;  // single-file mode: events.jsonl
  // Chunk mode mirrors the capture chunk series: capture-NNNNNN.jsonl next to
  // capture-NNNNNN.pcr. output_path is ignored when chunked is true.
  bool chunked = false;
  fs::path chunk_dir;
  uint32_t first_segment = 1;
  const Schema* schema = nullptr;
  std::string default_type_name;
  std::string proto_package;                 // fallback prefix for type_map names
  std::map<uint32_t, std::string> type_map;  // typed framing: id -> message name
  std::set<uint32_t> file_types;             // typed framing: ids that carry raw files
  fs::path files_dir;                        // where extracted blobs are written
  size_t max_inline_blob = 64ull * 1024;     // text/base64 fallback limit
  rec::Framing framing = rec::Framing::VarintLength;
  size_t max_queue_bytes = 64ull * 1024 * 1024;
  size_t max_frame = 16ull * 1024 * 1024;
  bool print_defaults = false;
  bool preserve_proto_names = false;
};

struct JsonChunkInfo {
  uint32_t segment = 0;
  std::string path;
  uint64_t events = 0;
  uint64_t bytes = 0;
};

struct JsonWriterStats {
  uint64_t events = 0;
  uint64_t messages = 0;
  uint64_t blobs = 0;
  uint64_t blob_bytes = 0;
  uint64_t unparsed = 0;
  uint64_t parse_errors = 0;
  uint64_t dropped_items = 0;
  uint64_t dropped_bytes = 0;
  uint64_t desynced_streams = 0;
  uint64_t skipped_data_items = 0;
  uint64_t bytes_written = 0;
  uint64_t chunks = 0;
  uint64_t chunk_open_errors = 0;
  bool failed = false;
  std::vector<JsonChunkInfo> chunk_infos;
};

// Item handed from the capture observer to the JSON writer. The segment index
// is the capture chunk that the recorder stored the item in, so both chunk
// series always carry the same index.
struct JsonQueueItem {
  ItemPtr item;
  uint32_t segment = 1;

  bool empty() const { return !item; }
  size_t accounted_bytes() const { return item ? item->accounted_bytes() : 0; }
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

  BoundedQueue<JsonQueueItem>& queue() { return queue_; }

  // Never blocks. If the queue is full, the item is dropped and the stream is
  // marked desynchronized so the worker stops attempting to reassemble frames
  // from a hole in the byte stream.
  PushResult submit(const ItemPtr& item, uint32_t segment = 1) {
    JsonQueueItem entry{item, segment};
    const PushResult result = queue_.push(std::move(entry));
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
  bool open_chunk(uint32_t segment, std::string& err);
  void close_chunk();
  void ensure_chunk(uint32_t segment);
  void emit_event_prefix(const char* event, uint32_t stream_id, int64_t ts_ns);
  void emit_message(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view type_name,
                    const google::protobuf::Message& message, uint32_t type_id = 0,
                    bool has_type = false);
  void emit_unparsed(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view reason,
                     const uint8_t* bytes, size_t len, uint32_t type_id = 0, bool has_type = false);
  void emit_blob(uint32_t stream_id, Direction dir, int64_t ts_ns, uint32_t type_id,
                 std::string_view payload);
  void handle_open(const CaptureItem& item);
  void handle_data(const CaptureItem& item);
  void handle_close(const CaptureItem& item);
  void handle_marker(const CaptureItem& item);
  StreamState* state_for(uint32_t stream_id, bool create);
  void prepare_channel(StreamState& state, uint32_t stream_id, bool outbound,
                       const std::string& type_name);
  bool take_desynced(uint32_t stream_id);

  JsonWriterConfig config_;
  BoundedQueue<JsonQueueItem> queue_;
  std::thread thread_;
  FILE* file_ = nullptr;
  std::string buffer_;
  std::atomic<uint64_t> events_{0};
  std::atomic<uint64_t> messages_{0};
  std::atomic<uint64_t> blobs_{0};
  std::atomic<uint64_t> blob_bytes_{0};
  std::atomic<uint64_t> unparsed_{0};
  std::atomic<uint64_t> parse_errors_{0};
  std::atomic<uint64_t> desynced_streams_{0};
  std::atomic<uint64_t> skipped_data_items_{0};
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> chunk_open_errors_{0};
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

  // Chunk bookkeeping (worker thread only, exposed through stats()).
  uint32_t current_segment_ = 0;
  std::string chunk_path_;
  uint64_t chunk_start_events_ = 0;
  uint64_t chunk_bytes_ = 0;
  mutable std::mutex chunk_mutex_;
  std::vector<JsonChunkInfo> chunk_infos_;
};

}  // namespace prp
