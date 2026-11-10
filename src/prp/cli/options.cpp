#include "prp/cli/options.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

namespace prp {
namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool parse_u64(std::string_view text, uint64_t& out) {
  if (text.empty()) return false;
  uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

std::string trim(std::string_view text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

bool parse_bool_value(std::string_view text, bool& out) {
  const std::string v = lower(text);
  if (v.empty() || v == "true" || v == "yes" || v == "on" || v == "1") {
    out = true;
    return true;
  }
  if (v == "false" || v == "no" || v == "off" || v == "0") {
    out = false;
    return true;
  }
  return false;
}

bool is_boolean_key(const std::string& key) {
  return key == "flat" || key == "durable" || key == "json" ||
         key == "json-print-defaults" || key == "json-preserve-names" || key == "truncate" ||
         key == "reconnect" || key == "strict" || key == "quiet" || key == "verbose";
}

// Reads a .ini file into CLI-style arguments. Keys are long option names
// without the leading "--"; '-' and '_' are interchangeable. Sections are
// accepted for readability but have no semantic meaning. The resulting
// arguments are meant to be parsed before the real command line so explicit
// CLI values win.
bool load_ini_config(const fs::path& path, std::vector<std::string>& args, std::string& err) {
  std::string content;
  if (!read_file(path, content, err)) {
    err = "cannot read config file '" + path.string() + "': " + err;
    return false;
  }
  const std::string prefix = "config file " + path.string() + ":";
  size_t pos = 0;
  size_t line_no = 0;
  while (pos < content.size()) {
    size_t end = content.find('\n', pos);
    if (end == std::string::npos) end = content.size();
    std::string raw = content.substr(pos, end - pos);
    pos = end + (end < content.size() ? 1 : 0);
    ++line_no;
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();

    const std::string text = trim(raw);
    if (text.empty() || text[0] == '#' || text[0] == ';') continue;
    if (text.front() == '[') {
      if (text.back() != ']') {
        err = prefix + std::to_string(line_no) + ": malformed section header";
        return false;
      }
      continue;
    }

    const size_t eq = text.find('=');
    if (eq == std::string::npos) {
      err = prefix + std::to_string(line_no) + ": expected 'key = value'";
      return false;
    }
    std::string key = trim(std::string_view(text).substr(0, eq));
    std::string value = trim(std::string_view(text).substr(eq + 1));
    if (key.empty()) {
      err = prefix + std::to_string(line_no) + ": missing key before '='";
      return false;
    }
    for (char& c : key) {
      if (c == '_') c = '-';
    }
    if (key == "config") {
      err = prefix + std::to_string(line_no) + ": 'config' cannot be set inside a config file";
      return false;
    }

    // Allow quoted values so leading/trailing spaces survive trimming.
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }

    if (is_boolean_key(key)) {
      bool enabled = false;
      if (!parse_bool_value(value, enabled)) {
        err = prefix + std::to_string(line_no) + ": invalid boolean for '" + key + "': " + value;
        return false;
      }
      if (key == "json") {
        args.push_back(enabled ? "--json=jsonl" : "--no-json");
      } else if (enabled) {
        args.push_back("--" + key);
      } else if (key == "flat") {
        args.push_back("--no-flat");
      }
      continue;
    }

    args.push_back("--" + key + "=" + value);
  }
  return true;
}

bool append_type_filter(std::string_view text, std::vector<std::string>& out, std::string& err) {
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    const size_t end = comma == std::string_view::npos ? text.size() : comma;
    std::string name(text.substr(start, end - start));
    const size_t first = name.find_first_not_of(" \t\r\n");
    const size_t last = name.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
      err = "empty message type in --filter";
      return false;
    }
    name = name.substr(first, last - first + 1);
    if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(std::move(name));
    if (comma == std::string_view::npos) return true;
    start = comma + 1;
  }
}

bool parse_log_level(std::string_view text, LogLevel& out) {
  const std::string v = lower(text);
  if (v == "debug") out = LogLevel::Debug;
  else if (v == "info") out = LogLevel::Info;
  else if (v == "warn" || v == "warning") out = LogLevel::Warn;
  else if (v == "error") out = LogLevel::Error;
  else return false;
  return true;
}

bool parse_listen(const std::string& text, std::string& host, uint16_t& port, std::string& err) {
  std::string h;
  std::string p;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      err = "invalid --listen value (expected [host]:port): " + text;
      return false;
    }
    h = text.substr(1, close - 1);
    p = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
      err = "invalid --listen value (expected host:port): " + text;
      return false;
    }
    h = text.substr(0, colon);
    p = text.substr(colon + 1);
  }
  uint64_t port_value = 0;
  if (!parse_u64(p, port_value) || port_value > 65535) {
    err = "invalid port in --listen: " + text;
    return false;
  }
  host = h.empty() ? "0.0.0.0" : h;
  port = static_cast<uint16_t>(port_value);
  return true;
}

void set_defaults_for_record(Options& o) {
  if (o.io_threads == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    o.io_threads = std::max(1u, std::min(4u, hw == 0 ? 2u : hw));
  }
}

bool parse_target(const std::string& text, std::string& host, uint16_t& port, std::string& err) {
  std::string h;
  std::string p;
  if (!text.empty() && text.front() == '[') {
    const auto close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() || text[close + 1] != ':') {
      err = "invalid --target value (expected [host]:port): " + text;
      return false;
    }
    h = text.substr(1, close - 1);
    p = text.substr(close + 2);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
      err = "invalid --target value (expected host:port): " + text;
      return false;
    }
    h = text.substr(0, colon);
    p = text.substr(colon + 1);
  }
  uint64_t port_value = 0;
  if (h.empty() || !parse_u64(p, port_value) || port_value == 0 || port_value > 65535) {
    err = "invalid --target value (expected host:port): " + text;
    return false;
  }
  host = std::move(h);
  port = static_cast<uint16_t>(port_value);
  return true;
}

bool parse_speed(const std::string& text, double& out) {
  std::string value = text;
  if (!value.empty() && (value.back() == 'x' || value.back() == 'X')) value.pop_back();
  if (value.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(value.c_str(), &end);
  if (errno != 0 || end != value.c_str() + value.size() || !std::isfinite(parsed) || parsed < 0.0) {
    return false;
  }
  out = parsed;
  return true;
}

bool parse_replay_direction(const std::string& text, ReplayDirection& out) {
  if (text == "in") out = ReplayDirection::In;
  else if (text == "out") out = ReplayDirection::Out;
  else if (text == "both") out = ReplayDirection::Both;
  else return false;
  return true;
}

void set_defaults_for_replay(Options& o) {
  if (o.io_threads == 0) {
    const unsigned hw = std::thread::hardware_concurrency();
    o.io_threads = std::max(1u, std::min(4u, hw == 0 ? 2u : hw));
  }
}

}  // namespace

bool parse_size(std::string_view raw, uint64_t& out) {
  std::string text(raw);
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
             text.end());
  if (text.empty()) return false;
  size_t pos = 0;
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos == 0) return false;
  uint64_t number = 0;
  if (!parse_u64(std::string_view(text).substr(0, pos), number)) return false;
  const std::string suffix = lower(text.substr(pos));
  uint64_t mult = 1;
  if (suffix.empty() || suffix == "b") mult = 1;
  else if (suffix == "k" || suffix == "kb" || suffix == "kib") mult = 1024ull;
  else if (suffix == "m" || suffix == "mb" || suffix == "mib") mult = 1024ull * 1024;
  else if (suffix == "g" || suffix == "gb" || suffix == "gib") mult = 1024ull * 1024 * 1024;
  else if (suffix == "t" || suffix == "tb" || suffix == "tib") mult = 1024ull * 1024 * 1024 * 1024;
  else return false;
  if (number > std::numeric_limits<uint64_t>::max() / mult) return false;
  out = number * mult;
  return true;
}

bool parse_duration(std::string_view raw, std::chrono::milliseconds& out) {
  std::string text(raw);
  text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
             text.end());
  if (text.empty()) return false;
  size_t pos = 0;
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos == 0) return false;
  uint64_t number = 0;
  if (!parse_u64(std::string_view(text).substr(0, pos), number)) return false;
  const std::string suffix = lower(text.substr(pos));
  uint64_t multiplier_ms = 1;
  if (suffix.empty() || suffix == "ms") multiplier_ms = 1;
  else if (suffix == "s") multiplier_ms = 1000;
  else if (suffix == "m") multiplier_ms = 60ull * 1000;
  else if (suffix == "h") multiplier_ms = 60ull * 60 * 1000;
  else return false;
  if (number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / multiplier_ms) return false;
  out = std::chrono::milliseconds(static_cast<int64_t>(number * multiplier_ms));
  return true;
}

bool parse_options(int argc, char** argv, Options& out, std::string& err) {
  out = Options{};
  if (argc < 2) {
    out.help = true;
    return true;
  }
  const std::string first = argv[1];
  if (first == "--help" || first == "-h" || first == "help") {
    out.help = true;
    return true;
  }
  if (first == "--version") {
    out.version = true;
    return true;
  }
  if (first != "record" && first != "inspect" && first != "replay" && first != "passthrough") {
    err =
        "unknown command '" + first +
        "' (expected 'record', 'replay', 'passthrough' or 'inspect')";
    return false;
  }
  out.command = first;

  // Pull --config out first, then parse config-derived arguments before the
  // explicit CLI arguments so CLI values take precedence.
  std::vector<std::string> args;
  fs::path config_path;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) {
        err = "missing value for --config";
        return false;
      }
      if (!config_path.empty()) {
        err = "--config may only be given once";
        return false;
      }
      config_path = argv[++i];
      continue;
    }
    constexpr std::string_view kConfigPrefix = "--config=";
    if (arg.rfind(kConfigPrefix, 0) == 0) {
      if (!config_path.empty()) {
        err = "--config may only be given once";
        return false;
      }
      config_path = arg.substr(kConfigPrefix.size());
      continue;
    }
    args.push_back(std::move(arg));
  }
  if (!config_path.empty()) {
    std::vector<std::string> config_args;
    if (!load_ini_config(config_path, config_args, err)) return false;
    config_args.insert(config_args.end(), args.begin(), args.end());
    args = std::move(config_args);
  }

  for (size_t i = 0; i < args.size(); ++i) {
    std::string arg = args[i];
    std::string key = arg;
    std::string value;
    bool has_value = false;
    const auto eq = arg.find('=');
    if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
      key = arg.substr(0, eq);
      value = arg.substr(eq + 1);
      has_value = true;
    }

    const auto need_value = [&](std::string& dst) -> bool {
      if (has_value) {
        dst = value;
        return true;
      }
      if (i + 1 >= args.size()) {
        err = "missing value for " + key;
        return false;
      }
      dst = args[++i];
      return true;
    };

    const auto need_size = [&](size_t& dst) -> bool {
      std::string text;
      if (!need_value(text)) return false;
      uint64_t parsed = 0;
      if (!parse_size(text, parsed) || parsed > std::numeric_limits<size_t>::max()) {
        err = "invalid size for " + key + ": " + text;
        return false;
      }
      dst = static_cast<size_t>(parsed);
      return true;
    };
    const auto need_u64 = [&](uint64_t& dst) -> bool {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_size(text, dst)) {
        err = "invalid number for " + key + ": " + text;
        return false;
      }
      return true;
    };
    const auto need_duration = [&](std::chrono::milliseconds& dst) -> bool {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_duration(text, dst)) {
        err = "invalid duration for " + key + ": " + text;
        return false;
      }
      return true;
    };
    if (key == "--listen") {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_listen(text, out.listen_host, out.listen_port, err)) return false;
    } else if (key == "--out") {
      std::string text;
      if (!need_value(text)) return false;
      out.out_dir = text;
    } else if (key == "--flat") {
      out.flat_output = true;
    } else if (key == "--no-flat") {
      out.flat_output = false;
    } else if (key == "--proto") {
      std::string text;
      if (!need_value(text)) return false;
      out.proto_file = text;
    } else if (key == "--proto-path") {
      std::string text;
      if (!need_value(text)) return false;
      out.proto_paths.push_back(text);
    } else if (key == "--message-type") {
      if (!need_value(out.message_type)) return false;
    } else if (key == "--filter" || key == "--filter-type") {
      std::string text;
      if (!need_value(text)) return false;
      if (!append_type_filter(text, out.type_filter, err)) return false;
    } else if (key == "--framing") {
      std::string text;
      if (!need_value(text)) return false;
      if (!rec::parse_framing(text, out.framing)) {
        err = "invalid --framing value (raw|varint-length): " + text;
        return false;
      }
    } else if (key == "--json") {
      if (has_value) {
        if (value == "off" || value == "false" || value == "0") out.json_enabled = false;
        else if (value == "jsonl" || value == "on" || value == "true" || value == "1") out.json_enabled = true;
        else {
          err = "invalid --json value (jsonl|off): " + value;
          return false;
        }
      } else {
        out.json_enabled = true;
      }
    } else if (key == "--json-file") {
      std::string text;
      if (!need_value(text)) return false;
      out.json_file = text;
      out.json_enabled = true;
    } else if (key == "--no-json") {
      out.json_enabled = false;
    } else if (key == "--json-print-defaults") {
      out.json_print_defaults = true;
    } else if (key == "--json-preserve-names") {
      out.json_preserve_names = true;
    } else if (key == "--max-frame") {
      if (!need_size(out.max_frame)) return false;
    } else if (key == "--recv-buffer") {
      if (!need_size(out.recv_buffer)) return false;
    } else if (key == "--queue-high-water") {
      if (!need_size(out.queue_bytes)) return false;
    } else if (key == "--overflow") {
      std::string text;
      if (!need_value(text)) return false;
      if (text == "block") out.overflow = OverflowPolicy::Block;
      else if (text == "drop") out.overflow = OverflowPolicy::Drop;
      else if (text == "disconnect") out.overflow = OverflowPolicy::Disconnect;
      else {
        err = "invalid --overflow value (block|drop|disconnect): " + text;
        return false;
      }
    } else if (key == "--write-batch") {
      if (!need_size(out.write_batch)) return false;
    } else if (key == "--fsync-interval") {
      if (!need_duration(out.fsync_interval)) return false;
    } else if (key == "--durable") {
      out.durable = true;
    } else if (key == "--rotate-size" || key == "--chunk-size") {
      if (!need_u64(out.rotate_bytes)) return false;
    } else if (key == "--rotate-interval" || key == "--chunk-interval" ||
               key == "--chunk-duration") {
      if (!need_duration(out.rotate_interval)) return false;
    } else if (key == "--io-threads") {
      uint64_t threads = 0;
      if (!need_u64(threads) || threads == 0 || threads > 256) {
        err = "invalid --io-threads value";
        return false;
      }
      out.io_threads = static_cast<unsigned>(threads);
    } else if (key == "--max-connections") {
      if (!need_size(out.max_connections)) return false;
    } else if (key == "--idle-timeout") {
      if (!need_duration(out.idle_timeout)) return false;
    } else if (key == "--send-type") {
      if (!need_value(out.send.type)) return false;
      out.send.enabled = true;
    } else if (key == "--send-content") {
      if (!need_value(out.send.content)) return false;
    } else if (key == "--send-content-file") {
      std::string text;
      if (!need_value(text)) return false;
      out.send.content = text;
      out.send.content_is_file = true;
    } else if (key == "--send-once-after") {
      if (!need_duration(out.send.once_after)) return false;
      out.send.has_once = true;
    } else if (key == "--send-every") {
      if (!need_duration(out.send.every)) return false;
      out.send.has_every = true;
    } else if (key == "--send-count") {
      if (!need_u64(out.send.count)) return false;
    } else if (key == "--rec") {
      std::string text;
      if (!need_value(text)) return false;
      out.input_file = text;
    } else if (key == "--dir") {
      std::string text;
      if (!need_value(text)) return false;
      out.input_dir = text;
    } else if (key == "--target") {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_target(text, out.target_host, out.target_port, err)) return false;
    } else if (key == "--speed") {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_speed(text, out.replay_speed)) {
        err = "invalid --speed value (expected a non-negative factor): " + text;
        return false;
      }
    } else if (key == "--direction") {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_replay_direction(text, out.replay_direction)) {
        err = "invalid --direction value (in|out|both): " + text;
        return false;
      }
    } else if (key == "--reconnect") {
      out.reconnect = true;
    } else if (key == "--connect-timeout") {
      if (!need_duration(out.connect_timeout)) return false;
    } else if (key == "--connect-retries") {
      if (!need_u64(out.connect_retries)) return false;
    } else if (key == "--retry-interval") {
      if (!need_duration(out.retry_interval)) return false;
    } else if (key == "--max-gap") {
      if (!need_duration(out.max_gap)) return false;
    } else if (key == "--forward-queue") {
      size_t queue_bytes = 0;
      if (!need_size(queue_bytes)) return false;
      if (queue_bytes < 1024 * 1024 || queue_bytes > 1024ull * 1024 * 1024) {
        err = "--forward-queue must be between 1 MiB and 1 GiB";
        return false;
      }
      out.forward_queue = queue_bytes;
    } else if (key == "--strict") {
      out.strict = true;
    } else if (key == "--truncate") {
      out.truncate_damaged = true;
    } else if (key == "--quiet") {
      out.log_level = LogLevel::Error;
    } else if (key == "--verbose") {
      out.log_level = LogLevel::Debug;
    } else if (key == "--log-level") {
      std::string text;
      if (!need_value(text)) return false;
      if (!parse_log_level(text, out.log_level)) {
        err = "invalid --log-level value (debug|info|warn|error): " + text;
        return false;
      }
    } else if (key == "--help" || key == "-h") {
      out.help = true;
      return true;
    } else {
      err = "unknown option '" + key + "'";
      return false;
    }
  }

  if (out.help) return true;

  if (out.command == "record" || out.command == "passthrough") {
    if (out.out_dir.empty()) {
      err = out.command + " requires --out <directory>";
      return false;
    }
    if (out.json_enabled && out.proto_file.empty()) {
      err = "--json requires --proto <file.proto>";
      return false;
    }
    if (!out.type_filter.empty() && out.message_type.empty()) {
      err = "--filter requires --message-type (the type of inbound streams)";
      return false;
    }
    if (out.command == "passthrough" &&
        (out.send.enabled || !out.send.content.empty() || out.send.has_once || out.send.has_every ||
         out.send.count > 0)) {
      err = "--send-* is not supported by passthrough (clients receive live target responses, not scripted messages)";
      return false;
    }
    if (out.send.enabled) {
      if (out.proto_file.empty()) {
        err = "--send-type requires --proto <file.proto>";
        return false;
      }
      if (out.send.type.empty()) {
        err = "--send-type is empty";
        return false;
      }
    }
    if (out.max_frame < 1 || out.max_frame > 1024ull * 1024 * 1024) {
      err = "--max-frame must be between 1 byte and 1 GiB";
      return false;
    }
    if (out.recv_buffer < 4096 || out.recv_buffer > 64ull * 1024 * 1024) {
      err = "--recv-buffer must be between 4 KiB and 64 MiB";
      return false;
    }
    if (out.queue_bytes < 1024 * 1024) {
      err = "--queue-high-water must be at least 1 MiB";
      return false;
    }
    if (out.write_batch < 4096 || out.write_batch > 256ull * 1024 * 1024) {
      err = "--write-batch must be between 4 KiB and 256 MiB";
      return false;
    }
    if (out.max_connections == 0) {
      err = "--max-connections must be > 0";
      return false;
    }
    if (out.command == "passthrough") {
      if (out.target_host.empty() || out.target_port == 0) {
        err = "passthrough requires --target <host:port>";
        return false;
      }
      if (!out.proto_paths.empty() && out.proto_file.empty()) {
        err = "--proto-path requires --proto";
        return false;
      }
      if (!out.input_file.empty() || !out.input_dir.empty()) {
        err = "passthrough does not use --rec or --dir";
        return false;
      }
      if (out.truncate_damaged) {
        err = "--truncate is only valid for inspect";
        return false;
      }
      if (out.replay_speed != 1.0) {
        err = "--speed is only valid for replay";
        return false;
      }
      if (out.replay_direction != ReplayDirection::In) {
        err = "--direction is only valid for replay";
        return false;
      }
      if (out.max_gap.count() != 0) {
        err = "--max-gap is only valid for replay";
        return false;
      }
      if (out.strict) {
        err = "--strict is only valid for replay";
        return false;
      }
      if (out.connect_retries == 0) {
        err = "--connect-retries must be > 0";
        return false;
      }
      if (out.connect_timeout.count() <= 0) {
        err = "--connect-timeout must be > 0";
        return false;
      }
      if (out.retry_interval.count() < 0) {
        err = "--retry-interval must not be negative";
        return false;
      }
      const auto wildcard = [](const std::string& host) {
        return host.empty() || host == "0.0.0.0" || host == "::" || host == "*";
      };
      if (out.target_port == out.listen_port &&
          (out.target_host == out.listen_host || wildcard(out.target_host) ||
           wildcard(out.listen_host))) {
        err = "--target must not point at the passthrough listener itself";
        return false;
      }
    }
    set_defaults_for_record(out);
  } else if (out.command == "replay") {
    if (out.input_file.empty() && out.input_dir.empty()) {
      err = "replay requires --rec <file.pcr> or --dir <session-dir>";
      return false;
    }
    if (!out.input_file.empty() && !out.input_dir.empty()) {
      err = "replay accepts either --rec or --dir, not both";
      return false;
    }
    if (out.target_host.empty() || out.target_port == 0) {
      err = "replay requires --target <host:port>";
      return false;
    }
    if (!out.proto_paths.empty() && out.proto_file.empty()) {
      err = "--proto-path requires --proto";
      return false;
    }
    if (out.connect_retries == 0) {
      err = "--connect-retries must be > 0";
      return false;
    }
    if (out.connect_timeout.count() <= 0) {
      err = "--connect-timeout must be > 0";
      return false;
    }
    if (out.retry_interval.count() < 0 || out.max_gap.count() < 0) {
      err = "--retry-interval and --max-gap must not be negative";
      return false;
    }
    // --truncate is an inspect-only repair flag; silently accepting it during
    // replay would be misleading.
    if (out.truncate_damaged) {
      err = "replay does not modify captures; --truncate is only valid for inspect";
      return false;
    }
    set_defaults_for_replay(out);
  } else {
    if (out.input_file.empty() && out.input_dir.empty()) {
      err = "inspect requires --rec <file> or --dir <session-dir>";
      return false;
    }
    if (!out.input_file.empty() && !out.input_dir.empty()) {
      err = "inspect accepts either --rec or --dir, not both";
      return false;
    }
  }
  return true;
}

void print_help() {
  std::printf(
      "protorecprep %s - protobuf stream recorder\n"
      "\n"
      "USAGE\n"
      "  protorecprep record [options]\n"
      "  protorecprep replay --rec <capture.pcr> --target <host:port> [options]\n"
      "  protorecprep replay --dir <session-dir> --target <host:port> [options]\n"
      "  protorecprep passthrough --target <host:port> [record options]\n"
      "  protorecprep inspect --rec <capture.pcr> [--truncate]\n"
      "  protorecprep inspect --dir <session-dir> [--truncate]\n"
      "\n"
      "RECORD OPTIONS\n"
      "  --config <file.ini>         Load options from an INI file (CLI overrides)\n"
      "  --listen <host:port>        Bind address (default 0.0.0.0:5000)\n"
      "  --out <dir>                 Target directory (required)\n"
      "  --flat                      Write into --out directly instead of a session subdirectory\n"
      "  --proto <file.proto>        Schema for JSON conversion and outbound messages\n"
      "  --proto-path <dir>          Additional import path (repeatable)\n"
      "  --message-type <pkg.Msg>    Message type for inbound streams\n"
      "  --filter <pkg.Msg[,..]>     Record only messages of these types (repeatable)\n"
      "  --framing <raw|varint-length>  Wire framing (default varint-length)\n"
      "  --json[=jsonl|off]          Enable JSONL sidecar (requires --proto)\n"
      "  --json-file <path>          JSONL data file (default <session>/events.jsonl)\n"
      "  --json-print-defaults       Include fields with default values\n"
      "  --json-preserve-names       Keep proto field names (snake_case)\n"
      "  --max-frame <size>          Max framed message size (default 16MiB)\n"
      "  --recv-buffer <size>        Per-connection read buffer (default 256KiB)\n"
      "  --queue-high-water <size>   Capture queue limit (default 256MiB)\n"
      "  --overflow <policy>         block|drop|disconnect (default block)\n"
      "  --write-batch <size>        Write batching threshold (default 1MiB)\n"
      "  --fsync-interval <dur>      Sync interval (default 1s, 0=only on close)\n"
      "  --durable                   Use F_FULLFSYNC on macOS\n"
      "  --rotate-size <size>        Optional capture chunk size cap (default off; alias --chunk-size)\n"
      "  --rotate-interval <dur>     Capture chunk duration (default 1m, 0=off; alias --chunk-interval)\n"
      "  --io-threads <n>            IO worker threads (default auto)\n"
      "  --max-connections <n>       Connection limit (default 1024)\n"
      "  --idle-timeout <dur>        Close idle connections (default off)\n"
      "  --send-type <pkg.Msg>       Message type to send to clients\n"
      "  --send-content <json>       JSON content for the outbound message\n"
      "  --send-content-file <file>  Read outbound JSON from file (@file also works)\n"
      "  --send-once-after <dur>     Send once after connect\n"
      "  --send-every <dur>          Send periodically\n"
      "  --send-count <n>            Total sends per connection (0=unlimited)\n"
      "\n"
      "REPLAY OPTIONS\n"
      "  --rec <file.pcr>            Replay a single capture file\n"
      "  --dir <session-dir>         Replay all capture-*.pcr chunks in order\n"
      "  --target <host:port>        Destination server (required)\n"
      "  --speed <factor>            Playback speed: 1.0 realtime, 0=max (default 1.0)\n"
      "  --direction <in|out|both>   Recorded direction to send (default in)\n"
      "  --proto <file.proto>        Optional schema digest validation\n"
      "  --proto-path <dir>          Additional import path (requires --proto)\n"
      "  --connect-timeout <dur>     Connect timeout per attempt (default 5s)\n"
      "  --connect-retries <n>       Total connect attempts (default 5)\n"
      "  --retry-interval <dur>      Delay between connect attempts (default 1s)\n"
      "  --reconnect                 Resume after a mid-stream connection loss\n"
      "  --max-gap <dur>             Cap idle gaps between records (default off)\n"
      "  --strict                    Treat damaged/inconsistent captures as fatal\n"
      "  --io-threads <n>            IO worker threads (default auto)\n"
      "\n"
      "PASSTHROUGH OPTIONS\n"
      "  All RECORD OPTIONS apply, except --send-* (no scripted client messages).\n"
      "  --target <host:port>        Destination server (required)\n"
      "  --forward-queue <size>      Per-stream backlog limit in both directions: inbound\n"
      "                              forward and response delivery to the client. Reads\n"
      "                              pause while a backlog is full; nothing is lost.\n"
      "                              (default 16MiB, 1MiB..1GiB)\n"
      "  --connect-timeout <dur>     Connect timeout per attempt (default 5s)\n"
      "  --connect-retries <n>       Total connect attempts (default 5)\n"
      "  --retry-interval <dur>      Delay between connect attempts (default 1s)\n"
      "  --reconnect                 Resume after a mid-stream connection loss\n"
      "  --io-threads <n>            IO worker threads (default auto)\n"
      "\n"
      "  Target responses are forwarded live to the client and are always\n"
      "  recorded (--filter does not apply to them). --message-type also\n"
      "  types the response channel; --send-* is not supported in passthrough.\n"
      "\n"
      "COMMON OPTIONS\n"
      "  --quiet | --verbose         Log level shortcuts\n"
      "  --log-level <lvl>           debug|info|warn|error\n"
      "\n"
      "Sizes accept KiB/MiB/GiB (binary); durations accept ms/s/m/h.\n"
      "\n"
      "CONFIG FILE\n"
      "  --config <file.ini> reads the same options from an INI file before the\n"
      "  command line, so explicit CLI values override config values. Repeatable\n"
      "  options (--filter, --proto-path) accumulate. Keys are long option names\n"
      "  without '--'; '-' and '_' are interchangeable, sections are ignored.\n"
      "  Relative paths (including proto) resolve against the working directory.\n"
      "  Example:\n"
      "\n"
      "    [record]\n"
      "    proto = proto/sample.proto\n"
      "    message_type = demo.TestRecord\n"
      "    out = recordings\n"
      "    json = true\n"
      "    chunk_interval = 1m\n",
      PRP_VERSION);
}

}  // namespace prp
