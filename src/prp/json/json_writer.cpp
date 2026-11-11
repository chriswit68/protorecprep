#include "prp/json/json_writer.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

#include "prp/util/base64.h"
#include "prp/util/bytes.h"
#include "prp/util/log.h"
#include "prp/util/sha256.h"
#include "prp/util/text.h"

namespace prp {
namespace {

bool has_magic(std::string_view data, std::string_view magic) {
  return data.size() >= magic.size() && data.substr(0, magic.size()) == magic;
}

bool is_utf8_text(std::string_view data) {
  size_t i = 0;
  while (i < data.size()) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    if (c < 0x80) {
      if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F) return false;
      ++i;
      continue;
    }
    size_t length = 0;
    uint32_t codepoint = 0;
    if ((c & 0xE0) == 0xC0) {
      length = 2;
      codepoint = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      length = 3;
      codepoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      length = 4;
      codepoint = c & 0x07;
    } else {
      return false;
    }
    if (i + length > data.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      const unsigned char continuation = static_cast<unsigned char>(data[i + k]);
      if ((continuation & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    i += length;
  }
  return true;
}

const char* detect_blob_kind(std::string_view data) {
  if (has_magic(data, std::string_view("\xd4\xc3\xb2\xa1", 4)) ||
      has_magic(data, std::string_view("\xa1\xb2\xc3\xd4", 4))) {
    return "pcap";
  }
  if (has_magic(data, std::string_view("\x0a\x0d\x0d\x0a", 4))) return "pcapng";
  if (data.size() >= 12 && has_magic(data, "RIFF") && data.substr(8, 4) == "WAVE") return "wav";
  if (has_magic(data, std::string_view("\x89PNG\r\n\x1a\n", 8))) return "png";
  if (has_magic(data, std::string_view("\xff\xd8\xff", 3))) return "jpeg";
  if (has_magic(data, "GIF87a") || has_magic(data, "GIF89a")) return "gif";
  if (has_magic(data, "%PDF-")) return "pdf";
  if (has_magic(data, std::string_view("PK\x03\x04", 4))) return "zip";
  if (has_magic(data, std::string_view("\x1f\x8b", 2))) return "gzip";
  if (has_magic(data, std::string_view("\x28\xb5\x2f\xfd", 4))) return "zstd";
  if (!data.empty() && is_utf8_text(data)) return "text";
  return "unknown";
}

const char* blob_extension(std::string_view kind) {
  if (kind == "pcap") return "pcap";
  if (kind == "pcapng") return "pcapng";
  if (kind == "wav") return "wav";
  if (kind == "png") return "png";
  if (kind == "jpeg") return "jpg";
  if (kind == "gif") return "gif";
  if (kind == "pdf") return "pdf";
  if (kind == "zip") return "zip";
  if (kind == "gzip") return "gz";
  if (kind == "zstd") return "zst";
  if (kind == "text") return "txt";
  return "bin";
}

std::string hex_prefix(std::string_view data, size_t max_bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  const size_t count = std::min(max_bytes, data.size());
  std::string out;
  out.reserve(count * 3 + 4);
  for (size_t i = 0; i < count; ++i) {
    if (i != 0) out.push_back(' ');
    const unsigned char byte = static_cast<unsigned char>(data[i]);
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  if (data.size() > count) out += " ...";
  return out;
}

}  // namespace

struct JsonWriter::StreamState {
  struct Channel {
    std::string type_name;
    const google::protobuf::Descriptor* descriptor = nullptr;
    std::unique_ptr<google::protobuf::Message> message;
    std::map<uint32_t, std::unique_ptr<google::protobuf::Message>> typed_messages;
    std::unique_ptr<FrameReader> reader;  // only for non-raw framing
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
  if (!config_.file_types.empty() && !config_.files_dir.empty()) {
    std::string dir_err;
    if (!ensure_directory(config_.files_dir, dir_err)) {
      started_.store(false);
      err = "cannot create blob directory " + config_.files_dir.string() + ": " + dir_err;
      return false;
    }
  }
  if (config_.chunked) {
    // Open the first chunk eagerly so an empty session still has a JSON file.
    if (!open_chunk(config_.first_segment, err)) {
      started_.store(false);
      return false;
    }
  } else {
    file_ = std::fopen(config_.output_path.c_str(), "wb");
    if (!file_) {
      started_.store(false);
      err = "cannot open " + config_.output_path.string() + ": " + std::strerror(errno);
      return false;
    }
    current_segment_ = config_.first_segment;
    chunk_path_ = config_.output_path.string();
  }
  if (config_.schema && config_.schema->loaded()) {
    factory_ = std::make_unique<google::protobuf::DynamicMessageFactory>(config_.schema->pool());
  }
  last_flush_ = std::chrono::steady_clock::now();
  thread_ = std::thread([this] { run(); });
  log_printf(LogLevel::Info, "json writer started: %s",
             config_.chunked ? (config_.chunk_dir / rec::segment_json_name(config_.first_segment)).c_str()
                             : config_.output_path.c_str());
  return true;
}

bool JsonWriter::open_chunk(uint32_t segment, std::string& err) {
  const fs::path path = config_.chunk_dir / rec::segment_json_name(segment);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    err = "cannot open " + path.string() + ": " + std::strerror(errno);
    chunk_open_errors_.fetch_add(1, std::memory_order_relaxed);
    log_printf(LogLevel::Warn, "json chunk %u: %s", segment, err.c_str());
    return false;
  }
  file_ = file;
  current_segment_ = segment;
  chunk_path_ = path.string();
  chunk_start_events_ = events_.load(std::memory_order_relaxed);
  chunk_bytes_ = 0;
  log_printf(LogLevel::Debug, "json chunk %s opened", path.c_str());
  return true;
}

void JsonWriter::close_chunk() {
  if (!file_) return;
  flush_buffer(true);
  std::fclose(file_);
  file_ = nullptr;
  JsonChunkInfo info;
  info.segment = current_segment_;
  info.path = chunk_path_;
  info.events = events_.load(std::memory_order_relaxed) - chunk_start_events_;
  info.bytes = chunk_bytes_;
  {
    std::lock_guard<std::mutex> lock(chunk_mutex_);
    chunk_infos_.push_back(info);
  }
  chunk_path_.clear();
  chunk_bytes_ = 0;
}

void JsonWriter::ensure_chunk(uint32_t segment) {
  if (segment == current_segment_) return;
  close_chunk();
  std::string err;
  if (!open_chunk(segment, err)) {
    // Remember the failed segment so a broken directory does not trigger one
    // attempt per item. A later chunk starts a fresh attempt.
    current_segment_ = segment;
  }
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
  s.blobs = blobs_.load(std::memory_order_relaxed);
  s.blob_bytes = blob_bytes_.load(std::memory_order_relaxed);
  s.unparsed = unparsed_.load(std::memory_order_relaxed);
  s.parse_errors = parse_errors_.load(std::memory_order_relaxed);
  s.dropped_items = queue_.dropped_items();
  s.dropped_bytes = queue_.dropped_bytes();
  s.desynced_streams = desynced_streams_.load(std::memory_order_relaxed);
  s.skipped_data_items = skipped_data_items_.load(std::memory_order_relaxed);
  s.bytes_written = bytes_written_.load(std::memory_order_relaxed);
  s.chunk_open_errors = chunk_open_errors_.load(std::memory_order_relaxed);
  s.failed = failed_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(chunk_mutex_);
    s.chunk_infos = chunk_infos_;
    s.chunks = chunk_infos_.size();
  }
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
  channel.typed_messages.clear();
  channel.resolved = true;
  if (!effective.empty() && config_.schema && config_.schema->loaded()) {
    channel.descriptor = config_.schema->find_message(effective);
    if (!channel.descriptor && !config_.proto_package.empty()) {
      channel.descriptor = config_.schema->find_message(config_.proto_package + "." + effective);
    }
    if (!channel.descriptor) {
      log_printf(LogLevel::Warn, "stream %u: message type '%s' not found in schema", stream_id,
                 effective.c_str());
    }
  }
  if (channel.descriptor && factory_) {
    channel.message.reset(factory_->GetPrototype(channel.descriptor)->New());
  }
  if (config_.framing != rec::Framing::Raw && !channel.reader) {
    channel.reader = std::make_unique<FrameReader>(config_.framing, config_.max_frame);
  }
}

void JsonWriter::emit_event_prefix(const char* event, uint32_t stream_id, int64_t ts_ns) {
  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"segment\":";
  buffer_ += std::to_string(current_segment_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"event\":\"";
  buffer_ += event;
  buffer_ += "\",\"stream\":";
  buffer_ += std::to_string(stream_id);
}

void JsonWriter::emit_message(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view type_name,
                              const google::protobuf::Message& message, uint32_t type_id, bool has_type) {
  std::string json;
  std::string err;
  if (!message_to_json(message, config_.print_defaults, config_.preserve_proto_names, json, err)) {
    emit_unparsed(stream_id, dir, ts_ns, "json conversion failed: " + err, nullptr, 0, type_id,
                  has_type);
    return;
  }
  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"segment\":";
  buffer_ += std::to_string(current_segment_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"stream\":";
  buffer_ += std::to_string(stream_id);
  buffer_ += ",\"dir\":\"";
  buffer_ += dir == Direction::In ? "in" : "out";
  buffer_ += "\",\"type\":\"";
  buffer_ += json_escape(type_name);
  buffer_ += "\"";
  if (has_type) {
    buffer_ += ",\"type_id\":";
    buffer_ += std::to_string(type_id);
  }
  buffer_ += ",\"message\":";
  buffer_ += json;
  buffer_ += "}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
  messages_.fetch_add(1, std::memory_order_relaxed);
}

void JsonWriter::emit_unparsed(uint32_t stream_id, Direction dir, int64_t ts_ns, std::string_view reason,
                               const uint8_t* bytes, size_t len, uint32_t type_id, bool has_type) {
  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"segment\":";
  buffer_ += std::to_string(current_segment_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"stream\":";
  buffer_ += std::to_string(stream_id);
  buffer_ += ",\"dir\":\"";
  buffer_ += dir == Direction::In ? "in" : "out";
  buffer_ += "\",\"unparsed\":true,\"reason\":\"";
  buffer_ += json_escape(reason);
  buffer_ += "\"";
  if (has_type) {
    buffer_ += ",\"type_id\":";
    buffer_ += std::to_string(type_id);
  }
  if (bytes && len > 0) {
    buffer_ += ",\"bytes_b64\":\"";
    buffer_ += base64_encode(bytes, len);
    buffer_ += "\"";
  }
  buffer_ += "}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
  unparsed_.fetch_add(1, std::memory_order_relaxed);
}

void JsonWriter::emit_blob(uint32_t stream_id, Direction dir, int64_t ts_ns, uint32_t type_id,
                           std::string_view payload) {
  // Blob channels prepend a u32 LE flag/counter (observed value 1) in front of
  // the complete file. Strip it when present so the extracted file is intact.
  std::string_view data = payload;
  uint32_t prefix = 0;
  bool has_prefix = false;
  if (data.size() >= 4) {
    const uint32_t candidate =
        get_u32le(reinterpret_cast<const uint8_t*>(data.data()));
    if (candidate == 1) {
      prefix = candidate;
      has_prefix = true;
      data.remove_prefix(4);
    }
  }

  const char* kind = detect_blob_kind(data);
  const std::string_view kind_view(kind);
  const std::string digest = Sha256::hex_of(data);

  std::string path;
  std::string write_error;
  if (!config_.files_dir.empty()) {
    const std::string filename = std::to_string(seq_ + 1) + "-" + kind + "." +
                                 blob_extension(kind_view);
    const fs::path target = config_.files_dir / filename;
    if (write_file_atomic(target, data, write_error)) {
      path = target.string();
    } else {
      log_printf(LogLevel::Warn, "stream %u: cannot write blob %s: %s", stream_id,
                 target.string().c_str(), write_error.c_str());
    }
  } else {
    write_error = "no blob directory configured";
  }

  buffer_ += "{\"seq\":";
  buffer_ += std::to_string(++seq_);
  buffer_ += ",\"segment\":";
  buffer_ += std::to_string(current_segment_);
  buffer_ += ",\"ts_ns\":";
  buffer_ += std::to_string(ts_ns);
  buffer_ += ",\"stream\":";
  buffer_ += std::to_string(stream_id);
  buffer_ += ",\"dir\":\"";
  buffer_ += dir == Direction::In ? "in" : "out";
  buffer_ += "\",\"type\":\"@file\",\"type_id\":";
  buffer_ += std::to_string(type_id);
  buffer_ += ",\"blob\":{\"kind\":\"";
  buffer_ += kind;
  buffer_ += "\",\"size\":";
  buffer_ += std::to_string(data.size());
  buffer_ += ",\"sha256\":\"";
  buffer_ += digest;
  buffer_ += "\"";
  if (has_prefix) {
    buffer_ += ",\"prefix\":";
    buffer_ += std::to_string(prefix);
  }
  if (!path.empty()) {
    buffer_ += ",\"path\":\"";
    buffer_ += json_escape(path);
    buffer_ += "\"";
  }
  if (!write_error.empty()) {
    buffer_ += ",\"error\":\"";
    buffer_ += json_escape(write_error);
    buffer_ += "\"";
  }
  // Keep the sidecar compact: only small text and small unknown blobs are
  // embedded. Everything else stays in the extracted file.
  if (kind_view == "text" && data.size() <= config_.max_inline_blob) {
    buffer_ += ",\"text\":\"";
    buffer_ += json_escape(data);
    buffer_ += "\"";
  } else if (kind_view == "unknown" && data.size() <= config_.max_inline_blob) {
    buffer_ += ",\"bytes_b64\":\"";
    buffer_ += base64_encode(data);
    buffer_ += "\"";
  }
  buffer_ += "}}\n";
  events_.fetch_add(1, std::memory_order_relaxed);
  blobs_.fetch_add(1, std::memory_order_relaxed);
  blob_bytes_.fetch_add(data.size(), std::memory_order_relaxed);
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
  if (!channel.reader) {
    channel.reader = std::make_unique<FrameReader>(config_.framing, config_.max_frame);
  }
  channel.reader->append(item.data.data(), item.data.size());

  std::string err;
  if (config_.framing == rec::Framing::TypedLengthLE) {
    FrameReader::Frame frame;
    while (true) {
      const FrameReader::Result result = channel.reader->next(frame, err);
      if (result == FrameReader::Result::NeedMore) break;
      if (result == FrameReader::Result::Error) {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
        emit_unparsed(item.stream_id, item.dir, item.ts_ns, "framing error: " + err, nullptr, 0);
        channel.reader->clear();
        break;
      }
      if (config_.file_types.find(frame.type_id) != config_.file_types.end()) {
        emit_blob(item.stream_id, item.dir, item.ts_ns, frame.type_id, frame.payload);
        continue;
      }
      const auto mapped = config_.type_map.find(frame.type_id);
      if (mapped == config_.type_map.end()) {
        emit_unparsed(item.stream_id, item.dir, item.ts_ns,
                      "unknown message type id " + std::to_string(frame.type_id),
                      reinterpret_cast<const uint8_t*>(frame.payload.data()), frame.payload.size(),
                      frame.type_id, true);
        continue;
      }
      const google::protobuf::Descriptor* descriptor = nullptr;
      if (config_.schema) {
        descriptor = config_.schema->find_message(mapped->second);
        if (!descriptor && !config_.proto_package.empty()) {
          descriptor = config_.schema->find_message(config_.proto_package + "." + mapped->second);
        }
      }
      if (!descriptor) {
        emit_unparsed(item.stream_id, item.dir, item.ts_ns,
                      "message type '" + mapped->second + "' not found in schema",
                      reinterpret_cast<const uint8_t*>(frame.payload.data()), frame.payload.size(),
                      frame.type_id, true);
        continue;
      }
      if (!factory_) {
        emit_unparsed(item.stream_id, item.dir, item.ts_ns, "schema not loaded",
                      reinterpret_cast<const uint8_t*>(frame.payload.data()), frame.payload.size(),
                      frame.type_id, true);
        continue;
      }
      std::unique_ptr<google::protobuf::Message>& message = channel.typed_messages[frame.type_id];
      if (!message) message.reset(factory_->GetPrototype(descriptor)->New());
      message->Clear();
      if (!message->ParseFromArray(frame.payload.data(), static_cast<int>(frame.payload.size()))) {
        parse_errors_.fetch_add(1, std::memory_order_relaxed);
        emit_unparsed(item.stream_id, item.dir, item.ts_ns,
                      "protobuf parse failed for " + mapped->second + " (first bytes " +
                          hex_prefix(frame.payload, 16) + ")",
                      reinterpret_cast<const uint8_t*>(frame.payload.data()), frame.payload.size(),
                      frame.type_id, true);
        continue;
      }
      emit_message(item.stream_id, item.dir, item.ts_ns, std::string(descriptor->full_name()),
                   *message, frame.type_id, true);
    }
    return;
  }

  std::string_view frame;
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
      std::string reason = "protobuf parse failed";
      if (!channel.type_name.empty()) reason += " for " + channel.type_name;
      reason += " (first bytes " + hex_prefix(frame, 16) + ")";
      emit_unparsed(item.stream_id, item.dir, item.ts_ns, reason,
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
      chunk_bytes_ += n;
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
    JsonQueueItem entry = queue_.pop();
    if (!entry.item) break;
    const ItemPtr& item = entry.item;

    // Mirror the capture chunk series. In single-file mode the segment is still
    // recorded per event, but the file never rotates.
    if (config_.chunked) {
      ensure_chunk(entry.segment);
    } else {
      current_segment_ = entry.segment;
    }

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
  if (config_.chunked) {
    close_chunk();
  } else {
    flush_buffer(true);
    if (file_) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }
  const JsonWriterStats s = stats();
  log_printf(LogLevel::Info,
             "json writer stopped: events=%" PRIu64 " messages=%" PRIu64 " blobs=%" PRIu64
             " unparsed=%" PRIu64 " dropped=%" PRIu64 " desynced=%" PRIu64 " chunks=%" PRIu64
             " bytes=%" PRIu64,
             s.events, s.messages, s.blobs, s.unparsed, s.dropped_items, s.desynced_streams,
             s.chunks, s.bytes_written);
}

}  // namespace prp
