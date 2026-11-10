#include "prp/app/record_app.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
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

bool filter_has(const std::vector<std::string>& filter, const std::string& type_name) {
  return !type_name.empty() && std::find(filter.begin(), filter.end(), type_name) != filter.end();
}

std::string uuid_hex(const std::array<uint8_t, 16>& uuid) {  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (uint8_t b : uuid) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

std::string meta_json(const Options& options, const Schema& schema, const fs::path& session_dir,
                      const fs::path& json_path, const std::string& session_id, const SendSpec& send,
                      const ForwardConfig& forward) {
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
  json += "  \"filter\": [";
  for (size_t i = 0; i < options.type_filter.size(); ++i) {
    if (i != 0) json += ", ";
    json += "\"" + json_escape(options.type_filter[i]) + "\"";
  }
  json += "],\n";
  json += "  \"json\": " + std::string(options.json_enabled ? "true" : "false") + ",\n";
  if (options.json_enabled) {
    json += "  \"json_file\": \"" + json_escape(json_path.string()) + "\",\n";
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
  json += "  \"unparsed\": " + std::to_string(s.unparsed) + ",\n";
  json += "  \"parse_errors\": " + std::to_string(s.parse_errors) + ",\n";
  json += "  \"dropped_items\": " + std::to_string(s.dropped_items) + ",\n";
  json += "  \"dropped_bytes\": " + std::to_string(s.dropped_bytes) + ",\n";
  json += "  \"desynced_streams\": " + std::to_string(s.desynced_streams) + ",\n";
  json += "  \"skipped_data_items\": " + std::to_string(s.skipped_data_items) + ",\n";
  json += "  \"bytes_written\": " + std::to_string(s.bytes_written) + ",\n";
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
  if (!options.proto_file.empty()) {
    if (!schema.load(options.proto_file, options.proto_paths, err)) {
      log_error("schema error: " + err);
      return 2;
    }
    if (!options.message_type.empty() && schema.find_message(options.message_type) == nullptr) {
      return fail("message type '" + options.message_type + "' not found in schema");
    }
    for (const std::string& type : options.type_filter) {
      if (schema.find_message(type) == nullptr) {
        return fail("filter message type '" + type + "' not found in schema");
      }
    }
  }

  std::string descriptor_set;
  if (schema.loaded()) {
    if (!schema.build_descriptor_set(descriptor_set, err)) return fail(err);
  }

  SendSpec send;
  if (options.send.enabled) {
    if (!schema.loaded()) {
      return fail("--send-type requires a loaded schema");
    }
    const google::protobuf::Descriptor* descriptor = schema.find_message(options.send.type);
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
    send.enabled = true;
    send.type_name = options.send.type;
    send.json_content = content;
    send.framed_bytes =
        options.framing == rec::Framing::VarintLength ? frame_message(serialized) : serialized;
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

  if (!options.type_filter.empty()) {
    if (!filter_has(options.type_filter, options.message_type) &&
        !(send.enabled && filter_has(options.type_filter, send.type_name))) {
      if (passthrough) {
        log_printf(LogLevel::Warn,
                   "type filter does not match inbound '%s'; inbound will not be recorded "
                   "(target responses are always recorded)",
                   options.message_type.c_str());
      } else {
        log_printf(LogLevel::Warn,
                   "type filter matches neither inbound '%s' nor outbound '%s'; "
                   "nothing will be recorded",
                   options.message_type.c_str(), send.type_name.c_str());
      }
    }
    std::string joined;
    for (const std::string& type : options.type_filter) {
      if (!joined.empty()) joined += ", ";
      joined += type;
    }
    log_printf(LogLevel::Info, "recording only message types: %s", joined.c_str());
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

  // JSON output is a single file for the whole session; the path is
  // configurable. Relative paths stay inside the session directory so all
  // artifacts travel together.
  fs::path json_path;
  if (options.json_enabled) {
    json_path = options.json_file.empty() ? fs::path("events.jsonl") : options.json_file;
    if (json_path.is_relative()) json_path = session_dir / json_path;
    if (!json_path.parent_path().empty() && !ensure_directory(json_path.parent_path(), err)) {
      return fail("cannot create JSON output directory: " + err);
    }
  }

  asio::io_context io;
  std::unique_ptr<JsonWriter> json;
  if (options.json_enabled) {
    JsonWriterConfig json_config;
    json_config.output_path = json_path;
    json_config.schema = &schema;
    json_config.default_type_name = options.message_type;
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
  server_config.default_type_name = options.message_type;
  server_config.type_filter = options.type_filter;
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
    server_config.forward.queue_limit = options.forward_queue;
  }

  Server server(io, server_config, recorder, json.get());

  const std::string meta = meta_json(options, schema, session_dir, json_path, session_id, send,
                                     server_config.forward);
  if (!write_file_atomic(session_dir / "meta.json", meta, err)) {
    log_error("could not write meta.json: " + err);
    return 2;
  }

  recorder.set_fatal_callback([&io, &server](std::string error) {
    log_error("fatal recorder error: " + error);
    asio::post(io, [&server] { server.shutdown(); });
  });

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
