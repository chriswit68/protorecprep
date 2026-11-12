#include "prp/app/record_app.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <unistd.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <google/protobuf/dynamic_message.h>

#include "prp/json/json_writer.h"
#include "prp/net/framer.h"
#include "prp/net/server.h"
#include "prp/proto/schema.h"
#include "prp/record/reader.h"
#include "prp/record/recorder.h"
#include "prp/util/fs.h"
#include "prp/util/log.h"
#include "prp/util/text.h"

namespace prp {
namespace {

namespace asio = boost::asio;
using ErrorCode = boost::system::error_code;

std::string uuid_hex(const std::array<uint8_t, 16>& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (uint8_t b : uuid) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

const google::protobuf::Descriptor* find_descriptor(const Schema& schema, const std::string& name,
                                                    const std::string& package) {
  const google::protobuf::Descriptor* descriptor = schema.find_message(name);
  if (!descriptor && !package.empty()) {
    descriptor = schema.find_message(package + "." + name);
  }
  return descriptor;
}

bool resolve_type_map(const std::map<uint32_t, std::string>& input, const std::string& package,
                      const Schema& schema, std::map<uint32_t, std::string>& out, std::string& err) {
  out.clear();
  for (const auto& [type_id, name] : input) {
    const google::protobuf::Descriptor* descriptor = find_descriptor(schema, name, package);
    if (!descriptor) {
      err = "type-map message '" + name + "' (id " + std::to_string(type_id) + ") not found in schema";
      return false;
    }
    const std::string full_name(descriptor->full_name());
    for (const auto& [existing_id, existing_name] : out) {
      if (existing_name == full_name) {
        err = "duplicate type-map message '" + full_name + "' (ids " +
              std::to_string(existing_id) + " and " + std::to_string(type_id) + ")";
        return false;
      }
    }
    out.emplace(type_id, full_name);
  }
  return true;
}

std::string meta_json(const Options& options, const Schema& schema, const fs::path& session_dir,
                      const fs::path& json_path, bool json_chunked, const fs::path& json_dir,
                      const std::string& session_id, const SendSpec& send,
                      const ForwardConfig& forward,
                      const std::map<uint32_t, std::string>& type_map,
                      const std::set<uint32_t>& file_types, const fs::path& files_dir) {
  const char* framing = rec::framing_name(options.framing);
  std::string json = "{\n";
  json += "  \"tool\": \"protorecprep\",\n";
  json += "  \"version\": \"" PRP_VERSION "\",\n";
  json += "  \"mode\": \"" + json_escape(options.command) + "\",\n";
  json += "  \"session_id\": \"" + json_escape(session_id) + "\",\n";
  json += "  \"started\": \"" + iso8601_now() + "\",\n";
  json += "  \"host\": \"" + json_escape(hostname()) + "\",\n";
  json += "  \"listen\": \"" + json_escape(options.listen_host) + ":" +
          std::to_string(options.listen_port) + "\",\n";
  json += "  \"directory\": \"" + json_escape(session_dir.string()) + "\",\n";
  json += "  \"framing\": \"" + std::string(framing) + "\",\n";
  json += "  \"proto\": \"" + json_escape(options.proto_file.string()) + "\",\n";
  json += "  \"proto_sha256\": \"" + schema.digest_hex() + "\",\n";
  json += "  \"message_type\": \"" + json_escape(options.message_type) + "\",\n";
  json += "  \"proto_package\": \"" + json_escape(options.proto_package) + "\",\n";
  std::map<uint32_t, std::string> combined_map = type_map;
  for (uint32_t type_id : file_types) combined_map.emplace(type_id, "@file");
  json += "  \"type_map\": {";
  size_t map_index = 0;
  for (const auto& [type_id, type_name] : combined_map) {
    if (map_index++ != 0) json += ", ";
    json += "\"" + std::to_string(type_id) + "\": \"" + json_escape(type_name) + "\"";
  }
  json += "},\n";
  if (!files_dir.empty()) {
    json += "  \"blob_dir\": \"" + json_escape(files_dir.string()) + "\",\n";
  }
  json += "  \"json\": " + std::string(options.json_enabled ? "true" : "false") + ",\n";
  if (options.json_enabled) {
    if (json_chunked) {
      json += "  \"json_chunks\": true,\n";
      json += "  \"json_dir\": \"" + json_escape(json_dir.string()) + "\",\n";
      json += "  \"json_chunk_pattern\": \"capture-%06u.jsonl\",\n";
    } else {
      json += "  \"json_chunks\": false,\n";
      json += "  \"json_file\": \"" + json_escape(json_path.string()) + "\",\n";
    }
  }
  json += "  \"max_frame\": " + std::to_string(options.max_frame) + ",\n";
  json += "  \"max_connections\": " + std::to_string(options.max_connections) + ",\n";
  json += "  \"overflow\": \"";
  json += options.overflow == OverflowPolicy::Block ? "block"
          : options.overflow == OverflowPolicy::Drop ? "drop"
                                                     : "disconnect";
  json += "\",\n";
  json += "  \"chunk_bytes\": " + std::to_string(options.rotate_bytes) + ",\n";
  json += "  \"chunk_interval_ms\": " + std::to_string(options.rotate_interval.count()) + ",\n";
  json += "  \"durable\": " + std::string(options.durable ? "true" : "false") + ",\n";
  json += "  \"send\": {";
  if (send.enabled) {
    json += "\"type\": \"" + json_escape(send.type_name) + "\", ";
    json += "\"once_after_ms\": " + std::to_string(send.has_once_after ? send.once_after.count() : -1) + ", ";
    json += "\"every_ms\": " + std::to_string(send.has_every ? send.every.count() : 0) + ", ";
    json += "\"count\": " + std::to_string(send.count);
  }
  json += "},\n";
  json += "  \"forward\": {\n";
  json += "    \"enabled\": " + std::string(forward.enabled ? "true" : "false") + ",\n";
  json += "    \"target\": \"" +
          json_escape(forward.enabled ? forward.host + ":" + std::to_string(forward.port) : "") +
          "\",\n";
  json += "    \"queue_bytes\": " + std::to_string(forward.queue_limit) + ",\n";
  json += "    \"reconnect\": " + std::string(forward.reconnect ? "true" : "false") + ",\n";
  json += "    \"link_connections\": " +
          std::string(forward.link_connections ? "true" : "false") + ",\n";
  json += "    \"connect_timeout_ms\": " + std::to_string(forward.connect_timeout.count()) + ",\n";
  json += "    \"connect_retries\": " + std::to_string(forward.connect_retries) + ",\n";
  json += "    \"retry_interval_ms\": " + std::to_string(forward.retry_interval.count()) + "\n";
  json += "  }\n";
  json += "}\n";
  return json;
}

void write_json_summary(const JsonWriter& writer, const fs::path& dir) {
  const JsonWriterStats s = writer.stats();
  std::string json = "{\n";
  json += "  \"events\": " + std::to_string(s.events) + ",\n";
  json += "  \"messages\": " + std::to_string(s.messages) + ",\n";
  json += "  \"blobs\": " + std::to_string(s.blobs) + ",\n";
  json += "  \"blob_bytes\": " + std::to_string(s.blob_bytes) + ",\n";
  json += "  \"unparsed\": " + std::to_string(s.unparsed) + ",\n";
  json += "  \"parse_errors\": " + std::to_string(s.parse_errors) + ",\n";
  json += "  \"dropped_items\": " + std::to_string(s.dropped_items) + ",\n";
  json += "  \"dropped_bytes\": " + std::to_string(s.dropped_bytes) + ",\n";
  json += "  \"desynced_streams\": " + std::to_string(s.desynced_streams) + ",\n";
  json += "  \"skipped_data_items\": " + std::to_string(s.skipped_data_items) + ",\n";
  json += "  \"bytes_written\": " + std::to_string(s.bytes_written) + ",\n";
  json += "  \"chunks_written\": " + std::to_string(s.chunks) + ",\n";
  json += "  \"chunk_open_errors\": " + std::to_string(s.chunk_open_errors) + ",\n";
  json += "  \"chunks\": [";
  for (size_t i = 0; i < s.chunk_infos.size(); ++i) {
    const JsonChunkInfo& chunk = s.chunk_infos[i];
    if (i != 0) json += ", ";
    json += "{\"segment\": " + std::to_string(chunk.segment) + ", \"file\": \"" +
            json_escape(chunk.path) + "\", \"events\": " + std::to_string(chunk.events) +
            ", \"bytes\": " + std::to_string(chunk.bytes) + "}";
  }
  json += "],\n";
  json += std::string("  \"failed\": ") + (s.failed ? "true" : "false") + ",\n";
  json += "  \"error\": \"" + json_escape(writer.last_error()) + "\"\n";
  json += "}\n";
  std::string err;
  if (!write_file_atomic(dir / "json-summary.json", json, err)) {
    log_printf(LogLevel::Warn, "could not write json-summary.json: %s", err.c_str());
  }
}

}  // namespace

int run_server_mode(const Options& options, std::string& err) {
  const bool passthrough = options.command == "passthrough";
  const std::string session_id = random_hex(16);

  auto fail = [&err](std::string message) {
    err = std::move(message);
    log_error(err);
    return 2;
  };

  fs::path session_dir = options.out_dir;
  if (!options.flat_output) {
    session_dir /= ("session-" + utc_timestamp_compact() + "-" + session_id.substr(0, 8));
  }
  if (!ensure_directory(session_dir, err)) return fail(err);

  Schema schema;
  std::string default_type_name;
  std::map<uint32_t, std::string> type_map;
  if (!options.proto_file.empty()) {
    if (!schema.load(options.proto_file, options.proto_paths, err)) {
      log_error("schema error: " + err);
      return 2;
    }
    if (!options.message_type.empty()) {
      const google::protobuf::Descriptor* descriptor =
          find_descriptor(schema, options.message_type, options.proto_package);
      if (!descriptor) {
        return fail("message type '" + options.message_type + "' not found in schema");
      }
      default_type_name = std::string(descriptor->full_name());
    }
    if (!options.type_map.empty() || !options.file_types.empty()) {
      if (!options.type_map.empty() &&
          !resolve_type_map(options.type_map, options.proto_package, schema, type_map, err)) {
        return fail(err);
      }
      std::string joined;
      for (const auto& [type_id, type_name] : type_map) {
        if (!joined.empty()) joined += ", ";
        joined += std::to_string(type_id) + "=" + type_name;
      }
      for (uint32_t type_id : options.file_types) {
        if (!joined.empty()) joined += ", ";
        joined += std::to_string(type_id) + "=@file";
      }
      log_printf(LogLevel::Info, "message type map: %s", joined.c_str());
    }
  }

  std::string descriptor_set;
  if (schema.loaded()) {
    if (!schema.build_descriptor_set(descriptor_set, err)) return fail(err);
  }

  if (!options.file_types.empty() && !options.json_enabled) {
    log_printf(LogLevel::Warn,
               "@file type-map entries require --json; blob payloads will not be extracted");
  }

  SendSpec send;
  if (options.send.enabled) {
    if (!schema.loaded()) {
      return fail("--send-type requires a loaded schema");
    }
    const google::protobuf::Descriptor* descriptor =
        find_descriptor(schema, options.send.type, options.proto_package);
    if (!descriptor) {
      return fail("send message type '" + options.send.type + "' not found in schema");
    }
    std::string content = options.send.content;
    if (options.send.content_is_file || (!content.empty() && content.front() == '@')) {
      const fs::path path = options.send.content_is_file ? fs::path(content) : fs::path(content.substr(1));
      if (!read_file(path, content, err)) return fail(err);
    }
    google::protobuf::DynamicMessageFactory factory(schema.pool());
    std::unique_ptr<google::protobuf::Message> message(factory.GetPrototype(descriptor)->New());
    if (!content.empty()) {
      if (!json_to_message(content, *message, false, err)) {
        return fail("invalid --send-content: " + err);
      }
    }
    std::string serialized;
    if (!message->SerializeToString(&serialized)) {
      return fail("could not serialize outbound message");
    }
    std::string framed;
    if (options.framing == rec::Framing::VarintLength) {
      framed = frame_message(serialized);
    } else if (options.framing == rec::Framing::TypedLengthLE) {
      uint32_t type_id = 0;
      bool found = false;
      for (const auto& [id, name] : type_map) {
        if (name == std::string(descriptor->full_name())) {
          type_id = id;
          found = true;
          break;
        }
      }
      if (!found) {
        return fail("send message type '" + std::string(descriptor->full_name()) +
                    "' has no --type-map entry for typed framing");
      }
      framed = frame_typed_message(type_id, serialized);
    } else {
      framed = serialized;
    }
    send.enabled = true;
    send.type_name = std::string(descriptor->full_name());
    send.json_content = content;
    send.framed_bytes = std::move(framed);
    send.once_after = options.send.once_after;
    send.has_once_after = options.send.has_once;
    send.every = options.send.every;
    send.has_every = options.send.has_every;
    send.count = options.send.count;
    if (!send.has_once_after && !send.has_every) {
      send.has_once_after = true;  // default: send once immediately after connect
      send.once_after = std::chrono::milliseconds(0);
    }
    if (send.has_every && send.every.count() <= 0) {
      return fail("--send-every must be greater than zero");
    }
  }

  RecorderConfig recorder_config;
  recorder_config.out_dir = session_dir;
  recorder_config.framing = options.framing;
  recorder_config.proto_sha256 = schema.digest();
  recorder_config.descriptor_set = std::move(descriptor_set);
  recorder_config.session_id = session_id;
  recorder_config.max_queue_bytes = options.queue_bytes;
  recorder_config.overflow = options.overflow;
  recorder_config.write_batch_bytes = options.write_batch;
  recorder_config.fsync_interval = options.fsync_interval;
  recorder_config.durable = options.durable;
  recorder_config.rotate_bytes = options.rotate_bytes;
  recorder_config.rotate_interval = options.rotate_interval;

  Recorder recorder(recorder_config);

  // JSON output defaults to one file per capture chunk (capture-NNNNNN.jsonl)
  // so events can be matched to their binary chunk. An explicit --json-file
  // switches to a single session-wide file instead.
  fs::path json_path;
  fs::path json_dir;
  const bool json_chunked = options.json_enabled && options.json_chunked;
  if (options.json_enabled) {
    if (json_chunked) {
      // Default: JSON chunks live next to the capture chunks in the session
      // directory. Only an explicit --json-dir is resolved relative to it, so
      // a relative --out is never joined to itself a second time.
      json_dir = resolve_subdir(session_dir, options.json_dir);
      if (!ensure_directory(json_dir, err)) {
        return fail("cannot create JSON chunk directory: " + err);
      }
    } else {
      json_path = resolve_subdir(
          session_dir, options.json_file.empty() ? fs::path("events.jsonl") : options.json_file);
      if (!json_path.parent_path().empty() && !ensure_directory(json_path.parent_path(), err)) {
        return fail("cannot create JSON output directory: " + err);
      }
    }
  }

  // Raw file/blob channels are extracted next to the JSON sidecar so all
  // artifacts of a session stay together.
  fs::path files_dir;
  if (options.json_enabled && !options.file_types.empty()) {
    files_dir = session_dir / "files";
    if (!ensure_directory(files_dir, err)) {
      return fail("cannot create blob output directory: " + err);
    }
  }

  asio::io_context io;
  std::unique_ptr<JsonWriter> json;
  if (options.json_enabled) {
    JsonWriterConfig json_config;
    json_config.output_path = json_path;
    json_config.chunked = json_chunked;
    json_config.chunk_dir = json_dir;
    json_config.schema = &schema;
    json_config.default_type_name = default_type_name;
    json_config.proto_package = options.proto_package;
    json_config.type_map = type_map;
    json_config.file_types = options.file_types;
    json_config.files_dir = files_dir;
    json_config.framing = options.framing;
    json_config.max_frame = options.max_frame;
    json_config.print_defaults = options.json_print_defaults;
    json_config.preserve_proto_names = options.json_preserve_names;
    json = std::make_unique<JsonWriter>(json_config);
  }

  ServerConfig server_config;
  server_config.listen_host = options.listen_host;
  server_config.listen_port = options.listen_port;
  server_config.framing = options.framing;
  server_config.default_type_name = default_type_name;
  server_config.recv_buffer = options.recv_buffer;
  server_config.max_frame = options.max_frame;
  server_config.max_connections = options.max_connections;
  server_config.idle_timeout = options.idle_timeout;
  server_config.send = send;
  if (passthrough) {
    server_config.forward.enabled = true;
    server_config.forward.host = options.target_host;
    server_config.forward.port = options.target_port;
    server_config.forward.connect_timeout = options.connect_timeout;
    server_config.forward.retry_interval = options.retry_interval;
    server_config.forward.connect_retries = options.connect_retries;
    server_config.forward.reconnect = options.reconnect;
    server_config.forward.link_connections = options.link_connections;
    server_config.forward.queue_limit = options.forward_queue;
  }

  Server server(io, server_config, recorder);

  const std::string meta = meta_json(options, schema, session_dir, json_path, json_chunked, json_dir,
                                     session_id, send, server_config.forward, type_map,
                                     options.file_types, files_dir);
  if (!write_file_atomic(session_dir / "meta.json", meta, err)) {
    log_error("could not write meta.json: " + err);
    return 2;
  }

  recorder.set_fatal_callback([&io, &server](std::string error) {
    log_error("fatal recorder error: " + error);
    asio::post(io, [&server] { server.shutdown(); });
  });

  // The recorder is authoritative for chunk boundaries. Every item it appends
  // is mirrored to the JSON sidecar with the segment index it was written to,
  // so capture-NNNNNN.pcr and capture-NNNNNN.jsonl stay aligned.
  if (json) {
    JsonWriter* json_ptr = json.get();
    recorder.set_item_observer([json_ptr](const ItemPtr& item, uint32_t segment) {
      json_ptr->submit(item, segment);
    });
  }

  if (!recorder.start(err)) {
    log_error("cannot start recorder: " + err);
    return 2;
  }
  if (json && !json->start(err)) {
    log_error("cannot start json writer: " + err);
    recorder.stop();
    return 2;
  }
  if (!server.open(err)) {
    log_error(err);
    if (json) json->stop();
    recorder.stop();
    return 2;
  }

  auto work = asio::make_work_guard(io);
  server.set_stopped_callback([&] {
    recorder.stop();
    if (json) {
      json->stop();
      write_json_summary(*json, session_dir);
    }
    work.reset();
    io.stop();
  });

  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&server](const ErrorCode& ec, int signal_number) {
    if (ec) return;
    log_printf(LogLevel::Info, "received signal %d, shutting down", signal_number);
    server.shutdown();
  });

  server.start();
  if (passthrough) {
    log_printf(LogLevel::Info, "recording to %s, forwarding inbound data to %s:%u",
               session_dir.string().c_str(), options.target_host.c_str(),
               static_cast<unsigned>(options.target_port));
  } else {
    log_printf(LogLevel::Info, "recording to %s", session_dir.string().c_str());
  }

  std::vector<std::thread> io_threads;
  for (unsigned i = 1; i < options.io_threads; ++i) {
    io_threads.emplace_back([&io] { io.run(); });
  }
  io.run();
  for (auto& thread : io_threads) thread.join();

  const RecorderStats stats = recorder.stats();
  if (stats.failed) {
    err = recorder.last_error();
    log_error("recording finished with errors: " + err);
    return 1;
  }
  if (passthrough) {
    const ForwardStats forward = server.forward_stats();
    log_printf(LogLevel::Info,
               "recording finished cleanly (%llu records, %s data); forwarded %llu stream(s), "
               "%s to target / %s responses to clients over %llu connection(s), %llu failure(s)",
               static_cast<unsigned long long>(stats.records), format_bytes(stats.data_bytes).c_str(),
               static_cast<unsigned long long>(forward.streams), format_bytes(forward.bytes).c_str(),
               format_bytes(forward.response_bytes).c_str(),
               static_cast<unsigned long long>(forward.connects),
               static_cast<unsigned long long>(forward.failures));
  } else {
    log_printf(LogLevel::Info, "recording finished cleanly (%llu records, %s data)",
               static_cast<unsigned long long>(stats.records), format_bytes(stats.data_bytes).c_str());
  }
  return 0;
}

int run_record(const Options& options, std::string& err) { return run_server_mode(options, err); }

int run_passthrough(const Options& options, std::string& err) {
  return run_server_mode(options, err);
}

int run_inspect(const Options& options, std::string& err) {
  std::vector<ScanStats> results;
  if (!options.input_file.empty()) {
    ScanStats stats;
    if (!scan_capture_file(options.input_file, stats, err)) return 2;
    stats.path = options.input_file;
    if (stats.truncated && options.truncate_damaged) {
      if (::truncate(options.input_file.c_str(), static_cast<off_t>(stats.valid_end)) != 0) {
        err = "truncate failed: " + std::string(std::strerror(errno));
        return 2;
      }
      log_printf(LogLevel::Info, "truncated %s at offset %llu", options.input_file.c_str(),
                 static_cast<unsigned long long>(stats.valid_end));
    }
    results.push_back(std::move(stats));
  } else {
    if (!recover_session_dir(options.input_dir, results, options.truncate_damaged, err)) return 2;
  }

  bool damaged = false;
  for (const auto& stats : results) {
    std::printf("%s\n", stats.path.string().c_str());
    std::printf("  format       : PCRP v%u, framing=%s\n", static_cast<unsigned>(stats.header.version),
                rec::framing_name(stats.header.framing));
    std::printf("  session      : %s\n", uuid_hex(stats.header.session_uuid).c_str());
    std::printf("  proto sha256 : %s\n", Sha256::hex(stats.header.proto_sha256).c_str());
    std::printf("  file size    : %s (header %llu bytes)\n", format_bytes(stats.file_size).c_str(),
                static_cast<unsigned long long>(stats.header_size));
    std::printf("  records      : %llu (data=%llu, open=%llu, close=%llu, marker=%llu)\n",
                static_cast<unsigned long long>(stats.records),
                static_cast<unsigned long long>(stats.data_records),
                static_cast<unsigned long long>(stats.open_records),
                static_cast<unsigned long long>(stats.close_records),
                static_cast<unsigned long long>(stats.marker_records));
    std::printf("  data         : %s\n", format_bytes(stats.data_bytes).c_str());
    if (stats.has_trailer) {
      std::printf("  trailer      : %s (records=%llu, bytes=%llu)\n",
                  stats.trailer.clean ? "clean" : "dirty",
                  static_cast<unsigned long long>(stats.trailer.records),
                  static_cast<unsigned long long>(stats.trailer.bytes));
    } else {
      std::printf("  trailer      : missing (interrupted recording)\n");
    }
    if (stats.truncated) {
      damaged = true;
      std::printf("  status       : DAMAGED at offset %llu: %s%s\n",
                  static_cast<unsigned long long>(stats.valid_end), stats.damage.c_str(),
                  options.truncate_damaged ? " (truncated)" : " (use --truncate to repair)");
    } else {
      std::printf("  status       : OK\n");
    }
  }
  return damaged && !options.truncate_damaged ? 1 : 0;
}

}  // namespace prp
