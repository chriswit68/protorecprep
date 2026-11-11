#include "prp/record/recorder.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "prp/util/crc32c.h"
#include "prp/util/log.h"
#include "prp/util/text.h"

namespace prp {
namespace {

// Reuse the shared helper so capture chunks and JSON chunks always share a stem.
std::string segment_name(uint32_t index) { return rec::segment_capture_name(index); }

}  // namespace

Recorder::Recorder(RecorderConfig config)
    : config_(std::move(config)), queue_(config_.max_queue_bytes, config_.overflow) {}

Recorder::~Recorder() { stop(); }

bool Recorder::start(std::string& err) {
  if (started_.exchange(true)) {
    err = "recorder already started";
    return false;
  }
  session_start_ns_ = now_wall_ns();
  batch_.reserve(config_.write_batch_bytes + 128 * 1024);
  if (!open_segment(err)) {
    started_.store(false);
    return false;
  }
  thread_ = std::thread([this] { run(); });
  log_printf(LogLevel::Info, "recorder started: %s (framing=%s)", config_.out_dir.c_str(),
             rec::framing_name(config_.framing));
  return true;
}

void Recorder::stop() {
  if (!started_.load()) return;
  queue_.close();
  if (thread_.joinable()) thread_.join();
  started_.store(false);
}

PushResult Recorder::submit(const ItemPtr& item) {
  const PushResult result = queue_.push(item);
  if (result == PushResult::Dropped || result == PushResult::Overflow) {
    // Log at most every 100 drops to keep the hot path cheap.
    const uint64_t dropped = queue_.dropped_items();
    if (dropped % 100 == 1) {
      log_printf(LogLevel::Warn, "capture queue full, dropped %" PRIu64 " items (%s buffered)", dropped,
                 format_bytes(queue_.buffered_bytes()).c_str());
    }
  }
  return result;
}

void Recorder::set_fatal_callback(std::function<void(std::string)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  fatal_callback_ = std::move(callback);
}

void Recorder::set_item_observer(ItemObserver observer) {
  std::lock_guard<std::mutex> lock(observer_mutex_);
  observer_ = std::move(observer);
}

RecorderStats Recorder::stats() const {
  RecorderStats s;
  s.records = total_records_.load(std::memory_order_relaxed);
  s.data_bytes = total_data_bytes_.load(std::memory_order_relaxed);
  s.segments = total_segments_.load(std::memory_order_relaxed);
  s.write_errors = write_errors_.load(std::memory_order_relaxed);
  s.failed = failed_.load(std::memory_order_relaxed);
  const QueueStats qs = queue_.stats();
  s.dropped_items = qs.dropped_items;
  s.dropped_bytes = qs.dropped_bytes;
  s.buffered_bytes = queue_.buffered_bytes();
  return s;
}

std::string Recorder::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return error_;
}

std::string Recorder::segment_path(uint32_t index) const {
  return (config_.out_dir / segment_name(index)).string();
}

bool Recorder::open_segment(std::string& err) {
  if (fd_ >= 0) {
    err = "internal: segment already open";
    return false;
  }
  ++segment_index_;
  const std::string path = segment_path(segment_index_);
  fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    err = "cannot create " + path + ": " + std::strerror(errno);
    return false;
  }

  rec::FileHeader header;
  header.version = rec::kFormatVersion;
  header.framing = config_.framing;
  header.start_wall_ns = now_wall_ns();
  header.proto_sha256 = config_.proto_sha256;
  header.descriptor_set = config_.descriptor_set;
  // Derive a stable session uuid from the session id if it is a hex string.
  if (config_.session_id.size() >= 32) {
    bool valid = true;
    for (size_t i = 0; i < 16; ++i) {
      const auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hexval(config_.session_id[i * 2]);
      const int lo = hexval(config_.session_id[i * 2 + 1]);
      if (hi < 0 || lo < 0) {
        valid = false;
        break;
      }
      header.session_uuid[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (!valid) header.session_uuid.fill(0);
  }

  const std::vector<uint8_t> encoded = rec::encode_file_header(header);
  if (!write_all(encoded.data(), encoded.size(), err)) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  segment_size_ = encoded.size();
  segment_records_ = 0;
  segment_data_bytes_ = 0;
  segment_start_ = std::chrono::steady_clock::now();
  last_sync_ = segment_start_;
  total_segments_.fetch_add(1, std::memory_order_relaxed);
  batch_.clear();

  // Re-emit stream metadata so each segment is self-describing on its own.
  if (segment_index_ > 1) {
    std::vector<uint8_t> open_payload;
    for (const auto& [stream_id, info] : active_streams_) {
      open_payload.clear();
      rec::append_stream_open_payload(open_payload, stream_id, now_wall_ns(), info.first, info.second);
      if (!append_record_unchecked(rec::RecordType::StreamOpen, open_payload, err)) {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
    }
  }
  log_printf(LogLevel::Debug, "capture segment %s opened", path.c_str());
  return true;
}

bool Recorder::close_segment(bool clean, std::string& err) {
  if (fd_ < 0) return true;
  if (clean) {
    std::vector<uint8_t> trailer_payload;
    rec::append_trailer_payload(trailer_payload, segment_records_, segment_data_bytes_, true, now_wall_ns());
    if (!append_record_unchecked(rec::RecordType::Trailer, trailer_payload, err)) {
      return false;
    }
  }
  if (!flush_batch(err)) return false;
  if (!ensure_synced(err, true)) return false;
  if (::close(fd_) != 0) {
    err = std::string("close failed: ") + std::strerror(errno);
    fd_ = -1;
    return false;
  }
  fd_ = -1;
  return true;
}

bool Recorder::write_all(const uint8_t* data, size_t n, std::string& err) {
  size_t written = 0;
  while (written < n) {
    const ssize_t rc = ::write(fd_, data + written, n - written);
    if (rc < 0) {
      if (errno == EINTR) continue;
      err = std::string("write failed: ") + std::strerror(errno);
      return false;
    }
    if (rc == 0) {
      err = "write returned 0";
      return false;
    }
    written += static_cast<size_t>(rc);
  }
  return true;
}

bool Recorder::flush_batch(std::string& err) {
  if (batch_.empty()) return true;
  if (!write_all(batch_.data(), batch_.size(), err)) return false;
  batch_.clear();
  if (batch_.capacity() > 8ull * 1024 * 1024) {
    batch_.shrink_to_fit();
    batch_.reserve(config_.write_batch_bytes + 128 * 1024);
  }
  return true;
}

bool Recorder::append_record_unchecked(rec::RecordType type, const std::vector<uint8_t>& payload,
                                       std::string& err) {
  if (payload.size() > rec::kMaxRecordPayload) {
    err = "record payload exceeds limit";
    return false;
  }
  const uint32_t crc = crc32c(payload.data(), payload.size());
  const auto prefix = rec::encode_record_prefix(type, static_cast<uint32_t>(payload.size()), crc);
  batch_.insert(batch_.end(), prefix.begin(), prefix.end());
  batch_.insert(batch_.end(), payload.begin(), payload.end());

  segment_size_ += rec::kRecordPrefixSize + payload.size();
  ++segment_records_;
  total_records_.fetch_add(1, std::memory_order_relaxed);
  if (type == rec::RecordType::Data) {
    const size_t n = rec::data_payload_bytes(payload.data(), payload.size());
    segment_data_bytes_ += n;
    total_data_bytes_.fetch_add(n, std::memory_order_relaxed);
  }
  return true;
}

bool Recorder::append_record(rec::RecordType type, const std::vector<uint8_t>& payload, std::string& err) {
  if (payload.size() > rec::kMaxRecordPayload) {
    err = "record payload exceeds limit";
    return false;
  }
  if (!maybe_rotate(rec::kRecordPrefixSize + payload.size(), err)) return false;
  return append_record_unchecked(type, payload, err);
}

bool Recorder::maybe_rotate(size_t needed, std::string& err) {
  if (segment_records_ == 0) return true;
  bool rotate = false;
  if (config_.rotate_bytes > 0 && segment_size_ + needed > config_.rotate_bytes) rotate = true;
  if (config_.rotate_interval.count() > 0 &&
      std::chrono::steady_clock::now() - segment_start_ >= config_.rotate_interval) {
    rotate = true;
  }
  if (!rotate) return true;

  if (!flush_batch(err)) return false;
  if (!close_segment(true, err)) return false;
  return open_segment(err);
}

bool Recorder::ensure_synced(std::string& err, bool force) {
  if (fd_ < 0) return true;
  if (!force && config_.fsync_interval.count() == 0) return true;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - last_sync_ < config_.fsync_interval) return true;

  if (!flush_batch(err)) return false;
#if defined(__APPLE__)
  if (config_.durable) {
    if (::fcntl(fd_, F_FULLFSYNC) != 0 && errno != EINVAL) {
      err = std::string("F_FULLFSYNC failed: ") + std::strerror(errno);
      return false;
    }
  }
#endif
  if (::fsync(fd_) != 0) {
    err = std::string("fsync failed: ") + std::strerror(errno);
    return false;
  }
  last_sync_ = now;
  return true;
}

void Recorder::fail(std::string error) {
  write_errors_.fetch_add(1, std::memory_order_relaxed);
  failed_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (error_.empty()) error_ = error;
  }
  log_printf(LogLevel::Error, "recorder failure: %s", error.c_str());
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  std::function<void(std::string)> callback;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback = fatal_callback_;
  }
  if (callback) callback(error);
}

void Recorder::run() {
  std::string err;
  uint64_t since_sync_check = 0;
  ItemObserver observer;
  {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    observer = observer_;
  }

  while (true) {
    ItemPtr item = queue_.pop();
    if (!item) break;
    if (failed_.load(std::memory_order_relaxed)) continue;

    payload_.clear();
    switch (item->kind) {
      case ItemKind::StreamOpen:
        rec::append_stream_open_payload(payload_, item->stream_id, item->ts_ns, item->peer, item->type_name);
        active_streams_[item->stream_id] = {item->peer, item->type_name};
        break;
      case ItemKind::Data:
        rec::append_data_payload(payload_, item->stream_id, item->dir, item->ts_ns, item->data.data(),
                                 item->data.size());
        break;
      case ItemKind::StreamClose:
        rec::append_stream_close_payload(payload_, item->stream_id,
                                         static_cast<CloseReason>(item->close_reason), item->ts_ns);
        active_streams_.erase(item->stream_id);
        break;
      case ItemKind::Marker:
        rec::append_marker_payload(payload_, item->ts_ns, item->peer);
        break;
    }

    const auto type = static_cast<rec::RecordType>(static_cast<uint8_t>(item->kind));
    if (!append_record(type, payload_, err)) {
      fail(err);
      continue;
    }
    // append_record may have rotated segments; segment_index_ now identifies the
    // chunk that actually contains this record. Mirror exactly that index to the
    // JSON sidecar so both chunk series stay aligned.
    if (observer) observer(item, segment_index_);

    if (batch_.size() >= config_.write_batch_bytes) {
      if (!flush_batch(err)) {
        fail(err);
        continue;
      }
    }

    if (++since_sync_check >= 64) {
      since_sync_check = 0;
      if (!ensure_synced(err, false)) {
        fail(err);
        continue;
      }
    }
  }

  if (!failed_.load(std::memory_order_relaxed)) {
    if (!close_segment(true, err)) fail(err);
  } else if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  write_summary(!failed_.load(std::memory_order_relaxed));
  const RecorderStats s = stats();
  log_printf(LogLevel::Info,
             "recorder stopped: records=%" PRIu64 " data=%s segments=%" PRIu64 " dropped=%" PRIu64,
             s.records, format_bytes(s.data_bytes).c_str(), s.segments, s.dropped_items);
}

void Recorder::write_summary(bool clean) {
  const RecorderStats s = stats();
  std::string json = "{\n";
  json += "  \"session_id\": \"" + json_escape(config_.session_id) + "\",\n";
  json += "  \"clean\": " + std::string(clean ? "true" : "false") + ",\n";
  json += "  \"start_wall_ns\": " + std::to_string(session_start_ns_) + ",\n";
  json += "  \"end_wall_ns\": " + std::to_string(now_wall_ns()) + ",\n";
  json += "  \"segments\": " + std::to_string(s.segments) + ",\n";
  json += "  \"records\": " + std::to_string(s.records) + ",\n";
  json += "  \"data_bytes\": " + std::to_string(s.data_bytes) + ",\n";
  json += "  \"dropped_items\": " + std::to_string(s.dropped_items) + ",\n";
  json += "  \"dropped_bytes\": " + std::to_string(s.dropped_bytes) + ",\n";
  json += "  \"write_errors\": " + std::to_string(s.write_errors) + ",\n";
  json += "  \"error\": \"" + json_escape(last_error()) + "\"\n";
  json += "}\n";
  std::string err;
  if (!write_file_atomic(config_.out_dir / "summary.json", json, err)) {
    log_printf(LogLevel::Warn, "could not write summary.json: %s", err.c_str());
  }
}

}  // namespace prp
