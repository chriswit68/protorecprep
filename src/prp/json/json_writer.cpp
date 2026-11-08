#include "prp/json/json_writer.h"

#include <cerrno>
#include <cinttypes>
#include <cstring>

#include "prp/util/base64.h"
#include "prp/util/log.h"
#include "prp/util/text.h"

namespace prp {

struct JsonWriter::StreamState {
  struct Channel {
    std::string type_name;
    const google::protobuf::Descriptor* descriptor = nullptr;
    std::unique_ptr<google::protobuf::Message> message;
    std::unique_ptr<FrameReader> reader;  // only for varint framing
    bool resolved = false;
  };

  std::string stream_type;
  Channel in;
  Channel out;
  bool desynced = false;
};

JsonWriter::JsonWriter(JsonWriterConfig config)
    : config_(std::move(config)), queue_(config_.max_queue_bytes, OverflowPolicy::Drop) {
  streams_.resize(256);
  buffer_.reserve(512 * 1024);
}

JsonWriter::~JsonWriter() { stop(); }

bool JsonWriter::start(std::string& err) {
  if (started_.exchange(true)) {
    err = "json writer already started";
    return false;
  }
  file_ = std::fopen(config_.output_path.c_str(), "wb");
  if (!file_) {
    started_.store(false);
    err = "cannot open " + config_.output_path.string() + ": " + std::strerror(errno);
    return false;
  }
  if (config_.schema && config_.schema->loaded()) {
    factory_ = std::make_unique<google::protobuf::DynamicMessageFactory>(config_.schema->pool());
  }
  last_flush_ = std::chrono::steady_clock::now();
  thread_ = std::thread([this] { run(); });
  log_printf(LogLevel::Info, "json writer started: %s", config_.output_path.c_str());
  return true;
}

void JsonWriter::stop() {
  if (!started_.load()) return;
  queue_.close();
  if (thread_.joinable()) thread_.join();
  started_.store(false);
}

JsonWriterStats JsonWriter::stats() const {
  JsonWriterStats s;
  s.events = events_.load(std::memory_order_relaxed);
  s.messages = messages_.load(std::memory_order_relaxed);
  s.unparsed = unparsed_.load(std::memory_order_relaxed);
  s.parse_errors = parse_errors_.load(std::memory_order_relaxed);
  s.dropped_items = queue_.dropped_items();
  s.dropped_bytes = queue_.dropped_bytes();
  s.desynced_streams = desynced_streams_.load(std::memory_order_relaxed);
  s.skipped_data_items = skipped_data_items_.load(std::memory_order_relaxed);
  s.bytes_written = bytes_written_.load(std::memory_order_relaxed);
  s.failed = failed_.load(std::memory_order_relaxed);
  return s;
}

bool JsonWriter::take_desynced(uint32_t stream_id) {
  std::lock_guard<std::mutex> lock(drop_mutex_);
  return desynced_.erase(stream_id) > 0;
}

std::string JsonWriter::last_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return error_;
}

JsonWriter::StreamState* JsonWriter::state_for(uint32_t stream_id, bool create) {
  if (stream_id >= streams_.size()) {
    if (!create) return nullptr;
    size_t new_size = streams_.size();
    while (new_size <= stream_id) new_size *= 2;
    streams_.resize(new_size);
  }
  auto& slot = streams_[stream_id];
  if (!slot && create) slot = std::make_unique<StreamState>();
  return slot.get();
}

void JsonWriter::prepare_channel(StreamState& state, uint32_t stream_id, bool outbound,
                                 const std::string& type_name) {
  StreamState::Channel& channel = outbound ? state.out : state.in;
  const std::string& effective = type_name.empty() ? state.stream_type : type_name;
  if (channel.resolved && channel.type_name == effective) return;

  channel.type_name = effective;
  channel.descriptor = nullptr;
  channel.message.reset();
  channel.resolved = true;
  if (!effective.empty() && config_.schema && config_.schema->loaded()) {
    channel.descriptor = config_.schema->find_message(effective);
    if (!channel.descriptor) {
      log_printf(LogLevel::Warn, "stream %u: message type '%s' not found in schema", stream_id,
                 effective.c_str());
    }
  }
  if (channel.descriptor && factory_) {
    channel.message.reset(factory_->GetPrototype(channel.descriptor)->New());
  }
  if (config_.framing == rec::Framing::VarintLength && !channel.reader) {
    channel.reader = std::make_unique<FrameReader>(config_.max_frame);
  }
}

void JsonWriter::emit_event_prefix(const char* event, uint32_t stream_id, int64_t ts_ns) {
  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"event\":\"";
  buffer_ += event;
  buffer_ += "\",\"stream\":";
  buffer_ += std::to_string(stream_id);
}

void JsonWriter::emit_message(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view type_name,
                              const google::protobuf::Message& message) {
  std::string json;
  std::string err;
  if (!message_to_json(message, config_.print_defaults, config_.preserve_proto_names, json, err)) {
    emit_unparsed(stream_id, dir, ts_ns, "json conversion failed: " + err, nullptr, 0);
    return;
  }
  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"stream\":";
  buffer_ += std::to_string(stream_id);
  buffer_ += ",\"dir\":\"";
  buffer_ += dir == Direction::In ? "in" : "out";
  buffer_ += "\",\"type\":\"";
  buffer_ += json_escape(type_name);
  buffer_ += "\",\"message\":";
  buffer_ += json;
  buffer_ += "}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
  messages_.fetch_add(1, std::memory_order_relaxed);
}

void JsonWriter::emit_unparsed(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view reason,
                               const uint8_t* bytes, size_t len) {
  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"stream\":";
  buffer_ += std::to_string(stream_id);
  buffer_ += ",\"dir\":\"";
  buffer_ += dir == Direction::In ? "in" : "out";
  buffer_ += "\",\"unparsed\":true,\"reason\":\"";
  buffer_ += json_escape(reason);
  buffer_ += "\"";
  if (bytes && len > 0) {
    buffer_ += ",\"bytes_b64\":\"";
    buffer_ += base64_encode(bytes, len);
    buffer_ += "\"";
  }
  buffer_ += "}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
  unparsed_.fetch_add(1, std::memory_order_relaxed);
}

void JsonWriter::handle_open(const CaptureItem& item) {
  StreamState* state = state_for(item.stream_id, true);
  state->stream_type = item.type_name.empty() ? config_.default_type_name : item.type_name;
  state->desynced = false;
  prepare_channel(*state, item.stream_id, false, state->stream_type);
  emit_event_prefix("stream_open", item.stream_id, item.ts_ns);
  buffer_ += ",\"peer\":\"";
  buffer_ += json_escape(item.peer);
  buffer_ += "\",\"type\":\"";
  buffer_ += json_escape(state->stream_type);
  buffer_ += "\"}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
}

void JsonWriter::handle_data(const CaptureItem& item) {
  StreamState* state = state_for(item.stream_id, true);
  if (state->desynced) {
    // A hole in the captured byte stream makes frame reassembly meaningless.
    // The binary capture remains authoritative; JSON decoding resumes only if
    // the stream is re-opened.
    skipped_data_items_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (config_.framing == rec::Framing::Raw) {
    emit_unparsed(item.stream_id, item.dir, item.ts_ns, "raw framing: no message boundaries", item.data.data(),
                  item.data.size());
    return;
  }

  const bool outbound = item.dir == Direction::Out;
  prepare_channel(*state, item.stream_id, outbound, item.type_name);
  StreamState::Channel& channel = outbound ? state->out : state->in;
  if (!channel.reader) channel.reader = std::make_unique<FrameReader>(config_.max_frame);
  channel.reader->append(item.data.data(), item.data.size());

  std::string_view frame;
  std::string err;
  while (true) {
    const FrameReader::Result result = channel.reader->next(frame, err);
    if (result == FrameReader::Result::NeedMore) break;
    if (result == FrameReader::Result::Error) {
      parse_errors_.fetch_add(1, std::memory_order_relaxed);
      emit_unparsed(item.stream_id, item.dir, item.ts_ns, "framing error: " + err, nullptr, 0);
      channel.reader->clear();
      break;
    }
    if (frame.empty() && !channel.message) {
      // Empty frame without a known message type: still record it.
      emit_unparsed(item.stream_id, item.dir, item.ts_ns, "empty frame", nullptr, 0);
      continue;
    }
    if (!channel.message) {
      emit_unparsed(item.stream_id, item.dir, item.ts_ns,
                    channel.type_name.empty() ? "no message type configured"
                                              : "message type not found in schema",
                    reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
      continue;
    }
    channel.message->Clear();
    if (!channel.message->ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
      parse_errors_.fetch_add(1, std::memory_order_relaxed);
      emit_unparsed(item.stream_id, item.dir, item.ts_ns, "protobuf parse failed",
                    reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
      continue;
    }
    emit_message(item.stream_id, item.dir, item.ts_ns, channel.type_name, *channel.message);
  }
}

void JsonWriter::handle_close(const CaptureItem& item) {
  StreamState* state = state_for(item.stream_id, false);
  if (state) {
    for (int outbound = 0; outbound <= 1; ++outbound) {
      StreamState::Channel& channel = outbound ? state->out : state->in;
      if (!channel.reader || channel.reader->buffered_bytes() == 0) continue;
      // Flush bytes from an incomplete frame so nothing silently disappears.
      const std::string_view leftover = channel.reader->buffered_view();
      emit_unparsed(item.stream_id, outbound ? Direction::Out : Direction::In, item.ts_ns,
                    "incomplete frame at stream close",
                    reinterpret_cast<const uint8_t*>(leftover.data()), leftover.size());
      channel.reader->clear();
    }
  }
  emit_event_prefix("stream_close", item.stream_id, item.ts_ns);
  buffer_ += ",\"reason\":\"";
  buffer_ += close_reason_name(static_cast<CloseReason>(item.close_reason));
  buffer_ += "\"}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
  if (item.stream_id < streams_.size()) streams_[item.stream_id].reset();
}

void JsonWriter::handle_marker(const CaptureItem& item) {
  emit_event_prefix("marker", 0, item.ts_ns);
  buffer_ += ",\"text\":\"";
  buffer_ += json_escape(item.peer);
  buffer_ += "\"}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
}

void JsonWriter::flush_buffer(bool force) {
  if (file_ && !failed_.load(std::memory_order_relaxed) && !buffer_.empty()) {
    const size_t n = std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
    if (n != buffer_.size()) {
      failed_.store(true, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_ = std::string("json write failed: ") + std::strerror(errno);
      }
      log_printf(LogLevel::Error, "json write failed (%s); continuing without JSON output",
                 std::strerror(errno));
    } else {
      bytes_written_.fetch_add(n, std::memory_order_relaxed);
      std::fflush(file_);
    }
  }
  buffer_.clear();
  last_flush_ = std::chrono::steady_clock::now();
  (void)force;
}

void JsonWriter::run() {
  int since_flush = 0;
  while (true) {
    ItemPtr item = queue_.pop();
    if (!item) break;

    if ((item->kind == ItemKind::Data || item->kind == ItemKind::StreamClose) &&
        take_desynced(item->stream_id)) {
      StreamState* state = state_for(item->stream_id, false);
      if (state) {
        state->desynced = true;
        if (state->in.reader) state->in.reader->clear();
        if (state->out.reader) state->out.reader->clear();
      }
      desynced_streams_.fetch_add(1, std::memory_order_relaxed);
      emit_unparsed(item->stream_id, item->dir, item->ts_ns,
                    "json sidecar queue overflow: stream bytes were lost", nullptr, 0);
      if (item->kind == ItemKind::Data) {
        skipped_data_items_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
    }

    switch (item->kind) {
      case ItemKind::StreamOpen: handle_open(*item); break;
      case ItemKind::Data: handle_data(*item); break;
      case ItemKind::StreamClose: handle_close(*item); break;
      case ItemKind::Marker: handle_marker(*item); break;
    }
    ++since_flush;
    if (buffer_.size() >= 256 * 1024 ||
        (since_flush >= 32 && buffer_.size() > 0 &&
         std::chrono::steady_clock::now() - last_flush_ > std::chrono::milliseconds(250))) {
      flush_buffer(false);
      since_flush = 0;
    }
  }
  flush_buffer(true);
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  const JsonWriterStats s = stats();
  log_printf(LogLevel::Info,
             "json writer stopped: events=%" PRIu64 " messages=%" PRIu64 " unparsed=%" PRIu64
             " dropped=%" PRIu64 " desynced=%" PRIu64 " bytes=%" PRIu64,
             s.events, s.messages, s.unparsed, s.dropped_items, s.desynced_streams, s.bytes_written);
}

}  // namespace prp
