#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "prp/core/item_queue.h"
#include "prp/record/format.h"
#include "prp/util/fs.h"
#include "prp/util/log.h"

namespace prp {

struct SendOptions {
  bool enabled = false;
  std::string type;
  std::string content;
  bool content_is_file = false;
  bool has_once = false;
  std::chrono::milliseconds once_after{0};
  bool has_every = false;
  std::chrono::milliseconds every{0};
  uint64_t count = 0;  // 0 = unlimited
};

enum class ReplayDirection { In, Out, Both };

struct Options {
  std::string command;  // "record", "replay", "passthrough" or "inspect"

  // Common
  LogLevel log_level = LogLevel::Info;

  // record
  std::string listen_host = "0.0.0.0";
  uint16_t listen_port = 5000;
  fs::path out_dir;
  bool flat_output = false;
  fs::path proto_file;
  std::vector<fs::path> proto_paths;
  std::string message_type;
  // Package prefix applied when a type-map name is not fully qualified. The
  // type-map itself is the only source of message ids for typed framing.
  std::string proto_package;
  std::map<uint32_t, std::string> type_map;  // message type id -> proto message name
  rec::Framing framing = rec::Framing::VarintLength;
  bool json_enabled = false;
  // JSONL data file override. Relative paths are resolved against the session
  // directory; empty selects "<session>/events.jsonl".
  fs::path json_file;
  bool json_print_defaults = false;
  bool json_preserve_names = false;
  size_t max_frame = 16ull * 1024 * 1024;
  size_t recv_buffer = 256ull * 1024;
  size_t queue_bytes = 256ull * 1024 * 1024;
  OverflowPolicy overflow = OverflowPolicy::Block;
  size_t write_batch = 1ull * 1024 * 1024;
  std::chrono::milliseconds fsync_interval{1000};
  bool durable = false;
  uint64_t rotate_bytes = 0;  // 0 = no size cap; chunks are time-based by default
  std::chrono::milliseconds rotate_interval{60 * 1000};  // capture chunk duration
  unsigned io_threads = 0;  // 0 = auto
  size_t max_connections = 1024;
  std::chrono::milliseconds idle_timeout{0};
  SendOptions send;

  // replay / passthrough
  std::string target_host;
  uint16_t target_port = 0;
  bool reconnect = false;
  uint64_t connect_retries = 5;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds retry_interval{1000};

  // replay
  double replay_speed = 1.0;  // 1.0 = realtime, 0 = as fast as possible
  ReplayDirection replay_direction = ReplayDirection::In;
  std::chrono::milliseconds max_gap{0};
  bool strict = false;

  // passthrough
  size_t forward_queue = 16ull * 1024 * 1024;  // per-stream backlog before reads pause

  // inspect / replay
  fs::path input_file;  // --rec
  fs::path input_dir;   // --dir
  bool truncate_damaged = false;  // inspect --truncate

  bool help = false;
  bool version = false;
};

bool parse_options(int argc, char** argv, Options& out, std::string& err);
void print_help();

bool parse_size(std::string_view text, uint64_t& out);
bool parse_duration(std::string_view text, std::chrono::milliseconds& out);

}  // namespace prp
