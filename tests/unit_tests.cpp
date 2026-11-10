#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "prp/cli/options.h"
#include "prp/core/capture_item.h"
#include "prp/core/item_queue.h"
#include "prp/json/json_writer.h"
#include "prp/net/framer.h"
#include "prp/proto/schema.h"
#include "prp/record/format.h"
#include "prp/record/reader.h"
#include "prp/record/recorder.h"
#include "prp/util/base64.h"
#include "prp/util/bytes.h"
#include "prp/util/crc32c.h"
#include "prp/util/fs.h"
#include "prp/util/sha256.h"
#include "prp/util/text.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
      ++g_failures;                                                                  \
    }                                                                                \
  } while (0)

#define CHECK_EQ(a, b)                                                               \
  do {                                                                               \
    const auto va = (a);                                                             \
    const auto vb = (b);                                                             \
    if (!(va == vb)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);    \
      ++g_failures;                                                                  \
    }                                                                                \
  } while (0)

std::filesystem::path make_temp_dir(const char* name) {
  const auto dir = std::filesystem::temp_directory_path() /
                   (std::string("prp-test-") + name + "-" + prp::random_hex(6));
  std::string err;
  CHECK(prp::ensure_directory(dir, err));
  return dir;
}

void test_crc32c() {
  const char* text = "123456789";
  CHECK_EQ(prp::crc32c(text, 9), 0xE3069283u);
  // Streaming update must produce the same result.
  const uint32_t a = prp::crc32c_extend(0, text, 4);
  CHECK_EQ(prp::crc32c_extend(a, text + 4, 5), 0xE3069283u);

  // Vectors that exercise the slice-by-8 software path under Linux/x86_64.
  CHECK_EQ(prp::crc32c("abcdefgh", 8), 0x0A9421B7u);
  CHECK_EQ(prp::crc32c("0123456789abcdef", 16), 0x42D3119Eu);
  const char* xs = "xxxxxxxxxxxxxxxxx";
  CHECK_EQ(prp::crc32c(xs, 17), 0xE366B97Bu);

  // Streaming across the 8-byte block boundary must match the one-shot result.
  const uint32_t b = prp::crc32c_extend(0, xs, 8);
  CHECK_EQ(prp::crc32c_extend(b, xs + 8, 9), 0xE366B97Bu);
}

void test_sha256() {
  CHECK_EQ(prp::Sha256::hex_of("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(prp::Sha256::hex_of(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

void test_base64() {
  CHECK_EQ(prp::base64_encode("hello"), std::string("aGVsbG8="));
  CHECK_EQ(prp::base64_encode(""), std::string(""));
  std::vector<uint8_t> out;
  CHECK(prp::base64_decode("aGVsbG8=", out));
  CHECK_EQ(std::string(out.begin(), out.end()), std::string("hello"));
  for (size_t n = 0; n < 40; ++n) {
    std::string data(n, '\0');
    for (size_t i = 0; i < n; ++i) data[i] = static_cast<char>((i * 37 + n) & 0xFF);
    std::vector<uint8_t> back;
    CHECK(prp::base64_decode(prp::base64_encode(data), back));
    CHECK_EQ(std::string(back.begin(), back.end()), data);
  }
}

void test_parsers() {
  uint64_t size = 0;
  CHECK(prp::parse_size("256MiB", size));
  CHECK_EQ(size, 256ull * 1024 * 1024);
  CHECK(prp::parse_size("1024", size));
  CHECK_EQ(size, 1024ull);
  CHECK(prp::parse_size("2G", size));
  CHECK_EQ(size, 2ull * 1024 * 1024 * 1024);
  CHECK(!prp::parse_size("abc", size));

  std::chrono::milliseconds duration{0};
  CHECK(prp::parse_duration("1500ms", duration));
  CHECK_EQ(duration.count(), 1500ll);
  CHECK(prp::parse_duration("2s", duration));
  CHECK_EQ(duration.count(), 2000ll);
  CHECK(prp::parse_duration("5m", duration));
  CHECK_EQ(duration.count(), 300000ll);
  CHECK(!prp::parse_duration("5x", duration));
}

void test_item_queue() {
  auto payload = [](size_t n) {
    return prp::make_data(1, prp::Direction::In, 0, std::vector<uint8_t>(n, 0x7F));
  };
  const size_t small_cost = payload(64)->accounted_bytes();
  const size_t big_cost = payload(256)->accounted_bytes();

  // Drop and Disconnect never block and account each discarded item.
  for (const auto policy : {prp::OverflowPolicy::Drop, prp::OverflowPolicy::Disconnect}) {
    prp::BoundedItemQueue queue(small_cost, policy);
    CHECK(queue.push(payload(64)) == prp::PushResult::Ok);
    const prp::PushResult expected =
        policy == prp::OverflowPolicy::Drop ? prp::PushResult::Dropped : prp::PushResult::Overflow;
    CHECK(queue.push(payload(64)) == expected);
    CHECK_EQ(queue.buffered_items(), size_t{1});
    CHECK_EQ(queue.stats().dropped_items, uint64_t{1});
    CHECK_EQ(queue.stats().dropped_bytes, static_cast<uint64_t>(small_cost));
    // An item larger than the queue is rejected as well.
    CHECK(queue.push(payload(256)) == expected);
    auto first = queue.pop();
    CHECK(first != nullptr);
    queue.close();
    CHECK(queue.pop() == nullptr);
  }

  // Block waits until the consumer makes room.
  {
    prp::BoundedItemQueue queue(small_cost, prp::OverflowPolicy::Block);
    CHECK(queue.push(payload(64)) == prp::PushResult::Ok);
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
      CHECK(queue.push(payload(64)) == prp::PushResult::Ok);
      pushed.store(true);
    });
    auto first = queue.pop();
    CHECK(first != nullptr);
    producer.join();
    CHECK(pushed.load());
    auto second = queue.pop();
    CHECK(second != nullptr);
    queue.close();
  }

  // Block admits an item larger than the whole queue instead of deadlocking.
  {
    CHECK(big_cost > small_cost);
    prp::BoundedItemQueue queue(small_cost, prp::OverflowPolicy::Block);
    CHECK(queue.push(payload(256)) == prp::PushResult::Ok);
    CHECK_EQ(queue.buffered_bytes(), big_cost);
    auto first = queue.pop();
    CHECK(first != nullptr);
    queue.close();
  }

  // Closing the queue releases a blocked producer.
  {
    prp::BoundedItemQueue queue(small_cost, prp::OverflowPolicy::Block);
    CHECK(queue.push(payload(64)) == prp::PushResult::Ok);
    std::atomic<int> result{0};
    std::thread producer([&] { result.store(static_cast<int>(queue.push(payload(64)))); });
    queue.close();
    producer.join();
    CHECK_EQ(result.load(), static_cast<int>(prp::PushResult::Closed));
    auto first = queue.pop();
    CHECK(first != nullptr);
    CHECK(queue.pop() == nullptr);
  }
}

void test_type_map_options() {
  auto parse = [](std::vector<std::string> args, prp::Options& out, std::string& err) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());
    return prp::parse_options(static_cast<int>(argv.size()), argv.data(), out, err);
  };

  {
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                 "--framing", "typed", "--proto-package", "demo",
                 "--type-map", "0=VersionInformation", "--type-map", "0x10=demo.Nested"},
                options, err));
    CHECK(options.framing == prp::rec::Framing::TypedLengthLE);
    CHECK_EQ(options.proto_package, std::string("demo"));
    CHECK_EQ(options.type_map.size(), size_t{2});
    CHECK_EQ(options.type_map.at(0), std::string("VersionInformation"));
    CHECK_EQ(options.type_map.at(16), std::string("demo.Nested"));
  }
  {
    // --message-type is rejected with typed framing.
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                  "--message-type", "demo.A", "--framing", "typed", "--type-map", "1=demo.A"},
                 options, err));
    CHECK(err.find("--message-type") != std::string::npos);
  }
  {
    // A type map requires a schema.
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--framing", "typed",
                  "--type-map", "1=demo.A"},
                 options, err));
    CHECK(err.find("--type-map requires --proto") != std::string::npos);
  }
  {
    // JSON with typed framing requires at least one mapping.
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                  "--framing", "typed", "--json"},
                 options, err));
    CHECK(err.find("--type-map") != std::string::npos);
  }
  {
    // Invalid id and duplicate ids are rejected.
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                  "--type-map", "notanumber=demo.A"},
                 options, err));
    CHECK(err.find("invalid message type id") != std::string::npos);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                  "--type-map", "1=demo.A", "--type-map", "1=demo.B"},
                 options, err));
    CHECK(err.find("duplicate message type id") != std::string::npos);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                  "--type-map", "1=demo.A", "--type-map", "2=demo.A"},
                 options, err));
    CHECK(err.find("duplicate message name") != std::string::npos);
  }
}

void test_chunk_and_json_options() {
  auto parse = [](std::vector<std::string> args, prp::Options& out, std::string& err) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());
    return prp::parse_options(static_cast<int>(argv.size()), argv.data(), out, err);
  };

  {
    // Defaults: one-minute chunks, no size cap, default JSON file name.
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "record", "--out", "/tmp/x"}, options, err));
    CHECK_EQ(options.rotate_interval.count(), int64_t{60 * 1000});
    CHECK_EQ(options.rotate_bytes, uint64_t{0});
    CHECK_EQ(options.json_file.string(), std::string());
    CHECK(!options.json_enabled);
  }
  {
    // Chunk aliases and a custom JSON data file.
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "record", "--out", "/tmp/x", "--proto", "sample.proto",
                 "--message-type", "demo.A", "--json-file", "custom.jsonl",
                 "--chunk-interval", "30s", "--chunk-size", "4KiB"},
                options, err));
    CHECK(options.json_enabled);
    CHECK_EQ(options.json_file.string(), std::string("custom.jsonl"));
    CHECK_EQ(options.rotate_interval.count(), int64_t{30 * 1000});
    CHECK_EQ(options.rotate_bytes, uint64_t{4096});
  }
  {
    // Legacy rotate names remain supported.
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "record", "--out", "/tmp/x", "--rotate-interval", "90s",
                 "--rotate-size", "2MiB"},
                options, err));
    CHECK_EQ(options.rotate_interval.count(), int64_t{90 * 1000});
    CHECK_EQ(options.rotate_bytes, uint64_t{2 * 1024 * 1024});
  }
  {
    // A JSON file implies JSON output, which still requires a schema.
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "record", "--out", "/tmp/x", "--json-file", "x.jsonl"},
                 options, err));
    CHECK(err.find("--json requires --proto") != std::string::npos);
  }
}

void test_replay_options() {
  auto parse = [](std::vector<std::string> args, prp::Options& out, std::string& err) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());
    return prp::parse_options(static_cast<int>(argv.size()), argv.data(), out, err);
  };

  {
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "replay", "--rec", "/tmp/a.pcr", "--target", "[::1]:6000",
                 "--speed", "2.5x", "--direction", "both", "--reconnect", "--connect-timeout",
                 "250ms", "--connect-retries", "3", "--retry-interval", "10ms", "--max-gap", "1s",
                 "--strict"},
                options, err));
    CHECK_EQ(options.command, std::string("replay"));
    CHECK_EQ(options.input_file.string(), std::string("/tmp/a.pcr"));
    CHECK_EQ(options.target_host, std::string("::1"));
    CHECK_EQ(options.target_port, uint16_t{6000});
    CHECK_EQ(options.replay_speed, 2.5);
    CHECK(options.replay_direction == prp::ReplayDirection::Both);
    CHECK(options.reconnect);
    CHECK_EQ(options.connect_timeout.count(), int64_t{250});
    CHECK_EQ(options.connect_retries, uint64_t{3});
    CHECK_EQ(options.retry_interval.count(), int64_t{10});
    CHECK_EQ(options.max_gap.count(), int64_t{1000});
    CHECK(options.strict);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "replay", "--dir", "/tmp/session", "--target", "localhost:1234",
                 "--direction", "out"},
                options, err));
    CHECK_EQ(options.input_dir.string(), std::string("/tmp/session"));
    CHECK(options.replay_direction == prp::ReplayDirection::Out);
    CHECK_EQ(options.replay_speed, 1.0);
  }
  {
    // --speed 0 means unthrottled.
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--speed", "0"},
                options, err));
    CHECK_EQ(options.replay_speed, 0.0);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr"}, options, err));
    CHECK(err.find("--target") != std::string::npos);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "replay", "--target", "h:1"}, options, err));
    CHECK(err.find("--rec") != std::string::npos);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--dir", "d", "--target", "h:1"},
                 options, err));
    CHECK(err.find("not both") != std::string::npos);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:0"}, options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h"}, options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", ":42"}, options, err));
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--speed", "-1"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--speed", "nan"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--speed", "inf"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--direction",
                  "sideways"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--truncate"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1",
                  "--connect-retries", "0"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1",
                  "--connect-timeout", "0ms"},
                 options, err));
    CHECK(!parse({"protorecprep", "replay", "--rec", "a.pcr", "--target", "h:1", "--proto-path",
                  "include"},
                 options, err));
  }
}

void test_passthrough_options() {
  auto parse = [](std::vector<std::string> args, prp::Options& out, std::string& err) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());
    return prp::parse_options(static_cast<int>(argv.size()), argv.data(), out, err);
  };

  {
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "127.0.0.1:7000",
                 "--forward-queue", "2MiB", "--reconnect", "--connect-timeout", "250ms",
                 "--connect-retries", "3", "--retry-interval", "10ms"},
                options, err));
    CHECK_EQ(options.command, std::string("passthrough"));
    CHECK_EQ(options.target_host, std::string("127.0.0.1"));
    CHECK_EQ(options.target_port, uint16_t{7000});
    CHECK_EQ(options.forward_queue, size_t{2} * 1024 * 1024);
    CHECK(options.reconnect);
    CHECK_EQ(options.connect_timeout.count(), int64_t{250});
    CHECK_EQ(options.connect_retries, uint64_t{3});
    CHECK_EQ(options.retry_interval.count(), int64_t{10});
  }
  {
    prp::Options options;
    std::string err;
    CHECK(parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1"}, options,
                err));
    CHECK_EQ(options.forward_queue, size_t{16} * 1024 * 1024);
  }
  {
    prp::Options options;
    std::string err;
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out"}, options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--target", "h:1"}, options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--rec",
                  "a.pcr"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--dir",
                  "/tmp/session"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1",
                  "--truncate"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--speed",
                  "2"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1",
                  "--direction", "out"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--max-gap",
                  "1s"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--strict"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--send-type",
                  "sample.Sample"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1",
                  "--send-every", "1s"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1", "--send-count",
                  "5"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "127.0.0.1:5000",
                  "--listen", "127.0.0.1:5000"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1",
                  "--forward-queue", "512KiB"},
                 options, err));
    CHECK(!parse({"protorecprep", "passthrough", "--out", "/tmp/out", "--target", "h:1",
                  "--forward-queue", "2GiB"},
                 options, err));
  }
  {
    // INI keys use the long option names without the leading dashes.
    const auto dir = make_temp_dir("passthrough-config");
    const auto ini = dir / "passthrough.ini";
    std::string err;
    CHECK(prp::write_file_atomic(ini,
                                 "[passthrough]\n"
                                 "out = /tmp/out\n"
                                 "target = 127.0.0.1:7100\n"
                                 "forward_queue = 3MiB\n"
                                 "reconnect = true\n"
                                 "connect_retries = 4\n",
                                 err));
    prp::Options options;
    CHECK(parse({"protorecprep", "passthrough", "--config", ini.string()}, options, err));
    CHECK_EQ(options.target_port, uint16_t{7100});
    CHECK_EQ(options.forward_queue, size_t{3} * 1024 * 1024);
    CHECK(options.reconnect);
    CHECK_EQ(options.connect_retries, uint64_t{4});
    std::filesystem::remove_all(dir);
  }
}

void test_config_file() {
  auto parse = [](std::vector<std::string> args, prp::Options& out, std::string& err) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) argv.push_back(arg.data());
    return prp::parse_options(static_cast<int>(argv.size()), argv.data(), out, err);
  };

  const auto dir = make_temp_dir("config");
  const auto config = dir / "rec.ini";
  std::string err;
  const std::string ini =
      "# example config\n"
      "[record]\n"
      "proto = sample.proto\n"
      "proto_path = /tmp/inc1\n"
      "proto-path = /tmp/inc2\n"
      "message_type = demo.Config\n"
      "type_map = 0=demo.A\n"
      "type_map = 1=demo.B\n"
      "json = true\n"
      "json_file = custom.jsonl\n"
      "chunk_interval = 45s\n"
      "chunk_size = 8KiB\n"
      "flat = true\n"
      "\n"
      "[extra]\n"
      "log_level = debug\n";
  CHECK(prp::write_file_atomic(config, ini, err));

  {
    prp::Options o;
    CHECK(parse({"protorecprep", "record", "--config", config.string(), "--out", "/tmp/x"}, o, err));
    CHECK_EQ(o.proto_file.string(), std::string("sample.proto"));
    CHECK_EQ(o.proto_paths.size(), size_t{2});
    CHECK_EQ(o.message_type, std::string("demo.Config"));
    CHECK_EQ(o.type_map.size(), size_t{2});
    CHECK_EQ(o.type_map.at(0), std::string("demo.A"));
    CHECK_EQ(o.type_map.at(1), std::string("demo.B"));
    CHECK(o.json_enabled);
    CHECK_EQ(o.json_file.string(), std::string("custom.jsonl"));
    CHECK_EQ(o.rotate_interval.count(), int64_t{45 * 1000});
    CHECK_EQ(o.rotate_bytes, uint64_t{8192});
    CHECK(o.flat_output);
    CHECK(o.log_level == prp::LogLevel::Debug);
  }
  {
    // Explicit CLI values override config values.
    prp::Options o;
    CHECK(parse({"protorecprep", "record", "--config=" + config.string(), "--out", "/tmp/x",
                 "--chunk-interval", "10s", "--message-type", "demo.Cli"},
                o, err));
    CHECK_EQ(o.rotate_interval.count(), int64_t{10 * 1000});
    CHECK_EQ(o.message_type, std::string("demo.Cli"));
  }
  {
    const auto off = dir / "off.ini";
    CHECK(prp::write_file_atomic(off, "flat = false\njson = off\n", err));
    prp::Options o;
    CHECK(parse({"protorecprep", "record", "--config", off.string(), "--out", "/tmp/x"}, o, err));
    CHECK(!o.flat_output);
    CHECK(!o.json_enabled);
  }
  {
    const auto bad = dir / "bad.ini";
    CHECK(prp::write_file_atomic(bad, "proto sample.proto\n", err));
    prp::Options o;
    CHECK(!parse({"protorecprep", "record", "--config", bad.string(), "--out", "/tmp/x"}, o, err));
    CHECK(err.find("expected 'key = value'") != std::string::npos);
  }
  {
    // Typed keys go through the normal option parser, so unknown names fail.
    const auto unknown = dir / "unknown.ini";
    CHECK(prp::write_file_atomic(unknown, "does_not_exist = 1\n", err));
    prp::Options o;
    CHECK(!parse({"protorecprep", "record", "--config", unknown.string(), "--out", "/tmp/x"}, o,
                 err));
    CHECK(err.find("unknown option") != std::string::npos);
  }
  {
    const auto replay_ini = dir / "replay.ini";
    CHECK(prp::write_file_atomic(replay_ini,
                                 "[replay]\n"
                                 "rec = a.pcr\n"
                                 "target = 127.0.0.1:7777\n"
                                 "speed = 0\n"
                                 "direction = out\n"
                                 "reconnect = true\n"
                                 "strict = true\n"
                                 "connect_retries = 2\n"
                                 "connect_timeout = 250ms\n"
                                 "max_gap = 2s\n",
                                 err));
    prp::Options o;
    CHECK(parse({"protorecprep", "replay", "--config", replay_ini.string()}, o, err));
    CHECK_EQ(o.input_file.string(), std::string("a.pcr"));
    CHECK_EQ(o.target_host, std::string("127.0.0.1"));
    CHECK_EQ(o.target_port, uint16_t{7777});
    CHECK_EQ(o.replay_speed, 0.0);
    CHECK(o.replay_direction == prp::ReplayDirection::Out);
    CHECK(o.reconnect);
    CHECK(o.strict);
    CHECK_EQ(o.connect_retries, uint64_t{2});
    CHECK_EQ(o.connect_timeout.count(), int64_t{250});
    CHECK_EQ(o.max_gap.count(), int64_t{2000});
  }
  {
    // Booleans set to false stay disabled.
    const auto replay_ini = dir / "replay-off.ini";
    CHECK(prp::write_file_atomic(replay_ini,
                                 "rec = a.pcr\n"
                                 "target = h:1\n"
                                 "reconnect = false\n"
                                 "strict = false\n",
                                 err));
    prp::Options o;
    CHECK(parse({"protorecprep", "replay", "--config", replay_ini.string()}, o, err));
    CHECK(!o.reconnect);
    CHECK(!o.strict);
  }
  std::filesystem::remove_all(dir);
}

void test_framer() {
  prp::FrameValidator validator(prp::rec::Framing::VarintLength, 4096);
  std::string err;

  std::string wire = prp::frame_message("abcdef") + prp::frame_message("");
  // Feed in awkward chunks, including a split varint.
  CHECK(validator.feed(reinterpret_cast<const uint8_t*>(wire.data()), 1, err));
  CHECK(validator.feed(reinterpret_cast<const uint8_t*>(wire.data()) + 1, 2, err));
  CHECK(validator.feed(reinterpret_cast<const uint8_t*>(wire.data()) + 3, wire.size() - 3, err));
  CHECK_EQ(validator.frames(), 2ull);
  CHECK(err.empty());

  prp::FrameValidator too_big(prp::rec::Framing::VarintLength, 4);
  std::string big = prp::frame_message("hello");
  CHECK(!too_big.feed(reinterpret_cast<const uint8_t*>(big.data()), big.size(), err));
  CHECK(!err.empty());

  // Malformed: unterminated varint (>10 bytes).
  prp::FrameValidator malformed(prp::rec::Framing::VarintLength, 1024);
  const uint8_t bad[11] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
  err.clear();
  CHECK(!malformed.feed(bad, sizeof(bad), err));

  prp::FrameReader reader(prp::rec::Framing::VarintLength, 4096);
  const std::string w1 = prp::frame_message("one");
  const std::string w2 = prp::frame_message("second");
  std::string data = w1 + w2.substr(0, 2);
  reader.append(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  std::string_view frame;
  CHECK(reader.next(frame, err) == prp::FrameReader::Result::Frame);
  CHECK_EQ(std::string(frame), std::string("one"));
  CHECK(reader.next(frame, err) == prp::FrameReader::Result::NeedMore);
  reader.append(reinterpret_cast<const uint8_t*>(w2.data()) + 2, w2.size() - 2);
  CHECK(reader.next(frame, err) == prp::FrameReader::Result::Frame);
  CHECK_EQ(std::string(frame), std::string("second"));
  CHECK(reader.next(frame, err) == prp::FrameReader::Result::NeedMore);

  // Typed framing carries the message type id in an 8-byte header.
  err.clear();
  prp::FrameValidator typed_validator(prp::rec::Framing::TypedLengthLE, 4096);
  std::string typed_wire = prp::frame_typed_message(0, "hello") +
                           prp::frame_typed_message(0x10, "world!");
  CHECK(typed_validator.feed(reinterpret_cast<const uint8_t*>(typed_wire.data()), 3, err));
  CHECK(typed_validator.feed(reinterpret_cast<const uint8_t*>(typed_wire.data()) + 3,
                             typed_wire.size() - 3, err));
  CHECK_EQ(typed_validator.frames(), 2ull);
  CHECK(err.empty());

  prp::FrameReader typed_reader(prp::rec::Framing::TypedLengthLE, 4096);
  const size_t first_typed = prp::kTypedHeaderSize + 5;
  typed_reader.append(reinterpret_cast<const uint8_t*>(typed_wire.data()), first_typed + 3);
  prp::FrameReader::Frame typed_frame;
  CHECK(typed_reader.next(typed_frame, err) == prp::FrameReader::Result::Frame);
  CHECK_EQ(typed_frame.type_id, 0u);
  CHECK_EQ(std::string(typed_frame.payload), std::string("hello"));
  CHECK_EQ(typed_frame.wire.size(), prp::kTypedHeaderSize + 5);
  CHECK(typed_reader.next(typed_frame, err) == prp::FrameReader::Result::NeedMore);
  typed_reader.append(reinterpret_cast<const uint8_t*>(typed_wire.data()) + first_typed + 3,
                      typed_wire.size() - first_typed - 3);
  CHECK(typed_reader.next(typed_frame, err) == prp::FrameReader::Result::Frame);
  CHECK_EQ(typed_frame.type_id, 0x10u);
  CHECK_EQ(std::string(typed_frame.payload), std::string("world!"));

  // Oversized typed frames are rejected.
  prp::FrameValidator typed_big(prp::rec::Framing::TypedLengthLE, 4);
  std::string big_typed = prp::frame_typed_message(1, "hello");
  err.clear();
  CHECK(!typed_big.feed(reinterpret_cast<const uint8_t*>(big_typed.data()), big_typed.size(), err));
  CHECK(!err.empty());
}

void test_file_header() {
  prp::rec::FileHeader header;
  header.framing = prp::rec::Framing::VarintLength;
  header.start_wall_ns = 123456789;
  header.session_uuid = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  header.proto_sha256 = prp::Sha256::digest("schema");
  header.descriptor_set = "descriptor-bytes";

  const auto encoded = prp::rec::encode_file_header(header);
  prp::rec::FileHeader decoded;
  size_t consumed = 0;
  std::string err;
  CHECK(prp::rec::decode_file_header(encoded.data(), encoded.size(), decoded, consumed, err));
  CHECK_EQ(consumed, encoded.size());
  CHECK_EQ(decoded.framing, prp::rec::Framing::VarintLength);
  CHECK_EQ(decoded.start_wall_ns, 123456789);
  CHECK(decoded.session_uuid == header.session_uuid);
  CHECK(decoded.proto_sha256 == header.proto_sha256);
  CHECK_EQ(decoded.descriptor_set, std::string("descriptor-bytes"));

  header.framing = prp::rec::Framing::TypedLengthLE;
  const auto typed_encoded = prp::rec::encode_file_header(header);
  prp::rec::FileHeader typed_decoded;
  consumed = 0;
  CHECK(prp::rec::decode_file_header(typed_encoded.data(), typed_encoded.size(), typed_decoded,
                                     consumed, err));
  CHECK_EQ(typed_decoded.framing, prp::rec::Framing::TypedLengthLE);

  auto corrupted = encoded;
  corrupted[corrupted.size() - 1] ^= 0xFF;
  prp::rec::FileHeader bad;
  CHECK(!prp::rec::decode_file_header(corrupted.data(), corrupted.size(), bad, consumed, err));
}

void test_recorder_roundtrip() {
  const auto dir = make_temp_dir("recorder");
  prp::RecorderConfig config;
  config.out_dir = dir;
  config.framing = prp::rec::Framing::VarintLength;
  config.session_id = prp::random_hex(16);
  config.rotate_bytes = 4096;  // force many segments
  config.fsync_interval = std::chrono::milliseconds(0);
  config.write_batch_bytes = 1024;
  config.proto_sha256 = prp::Sha256::digest("proto");

  prp::Recorder recorder(config);
  std::string err;
  CHECK(recorder.start(err));

  constexpr uint32_t kStream = 7;
  uint64_t expected_bytes = 0;
  recorder.submit(prp::make_stream_open(kStream, prp::now_wall_ns(), "127.0.0.1:1", "demo.TestRecord"));
  for (int i = 0; i < 50; ++i) {
    std::vector<uint8_t> payload(static_cast<size_t>(100 + i * 13));
    for (size_t k = 0; k < payload.size(); ++k) payload[k] = static_cast<uint8_t>(k + i);
    expected_bytes += payload.size();
    recorder.submit(prp::make_data(kStream, prp::Direction::In, prp::now_wall_ns(), std::move(payload)));
  }
  recorder.submit(prp::make_stream_close(kStream, prp::CloseReason::PeerClosed, prp::now_wall_ns()));
  recorder.stop();

  const prp::RecorderStats stats = recorder.stats();
  CHECK(!stats.failed);
  CHECK_EQ(stats.data_bytes, expected_bytes);
  CHECK(stats.segments > 1);

  std::vector<prp::ScanStats> results;
  CHECK(prp::recover_session_dir(dir, results, false, err));
  CHECK_EQ(results.size(), stats.segments);
  uint64_t data_bytes = 0;
  uint64_t opens = 0;
  for (const auto& scan : results) {
    CHECK(scan.header_ok);
    CHECK(!scan.truncated);
    CHECK(scan.has_trailer);
    CHECK(scan.trailer.clean);
    data_bytes += scan.data_bytes;
    opens += scan.open_records;
  }
  CHECK_EQ(data_bytes, expected_bytes);
  CHECK(opens >= results.size());  // stream metadata re-emitted per segment

  std::filesystem::remove_all(dir);
}

void test_capture_reader() {
  const auto dir = make_temp_dir("capture-reader");
  prp::RecorderConfig config;
  config.out_dir = dir;
  config.session_id = prp::random_hex(16);
  config.rotate_bytes = 0;
  config.fsync_interval = std::chrono::milliseconds(0);
  config.write_batch_bytes = 1024;
  config.proto_sha256 = prp::Sha256::digest("reader-proto");

  prp::Recorder recorder(config);
  std::string err;
  CHECK(recorder.start(err));
  const int64_t start = prp::now_wall_ns();
  recorder.submit(prp::make_stream_open(4, start, "127.0.0.1:9", "demo.Reader"));
  recorder.submit(prp::make_data(4, prp::Direction::In, start + 1000,
                                 std::vector<uint8_t>{'a', 'l', 'p', 'h', 'a'}));
  recorder.submit(prp::make_data(4, prp::Direction::Out, start + 2000,
                                 std::vector<uint8_t>{'b', 'e', 't', 'a'}));
  recorder.submit(prp::make_stream_close(4, prp::CloseReason::PeerClosed, start + 3000));
  recorder.stop();
  CHECK(!recorder.stats().failed);

  const auto file = dir / "capture-000001.pcr";
  CHECK(std::filesystem::exists(file));

  prp::rec::FileHeader header;
  size_t header_size = 0;
  CHECK(prp::read_capture_header(file, header, header_size, err));
  CHECK_EQ(header.proto_sha256, prp::Sha256::digest("reader-proto"));
  CHECK(header_size > prp::rec::kFileHeaderFixedSize);

  std::vector<prp::fs::path> files;
  CHECK(prp::list_capture_files(dir, files, err));
  CHECK_EQ(files.size(), size_t{1});
  CHECK_EQ(files[0].filename().string(), std::string("capture-000001.pcr"));

  {
    prp::CaptureReader reader;
    CHECK(reader.open(file, err));
    std::vector<std::string> data;
    std::vector<prp::Direction> directions;
    uint32_t opened = 0;
    uint32_t closed = 0;
    uint32_t trailers = 0;
    std::string peer;
    std::string type;
    while (true) {
      prp::CaptureRecord record;
      std::string detail;
      const prp::ReadStatus status = reader.next(record, detail);
      if (status == prp::ReadStatus::Eof) break;
      CHECK(status == prp::ReadStatus::Record);
      switch (record.type) {
        case prp::rec::RecordType::StreamOpen:
          ++opened;
          CHECK_EQ(record.stream_id, uint32_t{4});
          peer = record.peer;
          type = record.type_name;
          break;
        case prp::rec::RecordType::Data:
          data.emplace_back(record.data.begin(), record.data.end());
          directions.push_back(record.dir);
          break;
        case prp::rec::RecordType::StreamClose:
          ++closed;
          CHECK_EQ(record.close_reason, static_cast<uint8_t>(prp::CloseReason::PeerClosed));
          break;
        case prp::rec::RecordType::Trailer:
          ++trailers;
          break;
        case prp::rec::RecordType::Marker:
          break;
      }
    }
    CHECK_EQ(opened, uint32_t{1});
    CHECK_EQ(closed, uint32_t{1});
    CHECK_EQ(trailers, uint32_t{1});
    CHECK_EQ(peer, std::string("127.0.0.1:9"));
    CHECK_EQ(type, std::string("demo.Reader"));
    CHECK_EQ(data.size(), size_t{2});
    CHECK_EQ(data[0], std::string("alpha"));
    CHECK_EQ(data[1], std::string("beta"));
    CHECK(directions[0] == prp::Direction::In);
    CHECK(directions[1] == prp::Direction::Out);
  }

  // A damaged tail is reported once, then the reader reports EOF.
  {
    std::FILE* f = std::fopen(file.c_str(), "ab");
    CHECK(f != nullptr);
    const uint8_t junk[10] = {2, 0x40, 0, 0, 0, 1, 2, 3, 4, 5};
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);
  }
  {
    prp::CaptureReader reader;
    CHECK(reader.open(file, err));
    uint32_t valid = 0;
    bool damaged = false;
    for (int i = 0; i < 100; ++i) {
      prp::CaptureRecord record;
      std::string detail;
      const prp::ReadStatus status = reader.next(record, detail);
      if (status == prp::ReadStatus::Record) {
        ++valid;
        continue;
      }
      if (status == prp::ReadStatus::Damaged) {
        damaged = true;
        CHECK(!detail.empty());
        continue;
      }
      CHECK(status == prp::ReadStatus::Eof);
      break;
    }
    CHECK(damaged);
    CHECK_EQ(valid, uint32_t{5});
  }

  std::filesystem::remove_all(dir);
}

void test_truncation_recovery() {
  const auto dir = make_temp_dir("recovery");
  prp::RecorderConfig config;
  config.out_dir = dir;
  config.session_id = prp::random_hex(16);
  config.rotate_bytes = 0;
  config.fsync_interval = std::chrono::milliseconds(0);

  prp::Recorder recorder(config);
  std::string err;
  CHECK(recorder.start(err));
  recorder.submit(prp::make_data(1, prp::Direction::In, prp::now_wall_ns(), std::vector<uint8_t>(64, 0xAB)));
  recorder.stop();

  const auto file = dir / "capture-000001.pcr";
  CHECK(std::filesystem::exists(file));
  const uint64_t good_size = std::filesystem::file_size(file);
  {
    std::FILE* f = std::fopen(file.c_str(), "ab");
    CHECK(f != nullptr);
    const uint8_t junk[12] = {2, 0x40, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7};  // partial record
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);
  }

  prp::ScanStats damaged;
  CHECK(prp::scan_capture_file(file, damaged, err));
  CHECK(damaged.truncated);
  CHECK_EQ(damaged.valid_end, good_size);

  std::vector<prp::ScanStats> results;
  CHECK(prp::recover_session_dir(dir, results, true, err));
  CHECK_EQ(results.size(), 1u);
  CHECK_EQ(std::filesystem::file_size(file), good_size);

  prp::ScanStats repaired;
  CHECK(prp::scan_capture_file(file, repaired, err));
  CHECK(!repaired.truncated);
  CHECK(repaired.has_trailer);

  std::filesystem::remove_all(dir);
}

void test_schema(const char* proto_path) {
  prp::Schema schema;
  std::string err;
  CHECK(schema.load(proto_path, {}, err));
  CHECK(schema.loaded());
  CHECK(schema.find_message("demo.TestRecord") != nullptr);
  CHECK(schema.find_message("demo.Missing") == nullptr);

  std::string descriptor_set;
  CHECK(schema.build_descriptor_set(descriptor_set, err));
  CHECK(!descriptor_set.empty());

  const auto* descriptor = schema.find_message("demo.TestRecord");
  CHECK(descriptor != nullptr);
  google::protobuf::DynamicMessageFactory factory(schema.pool());
  std::unique_ptr<google::protobuf::Message> message(factory.GetPrototype(descriptor)->New());
  CHECK(prp::json_to_message("{\"seq\":42,\"name\":\"abc\",\"nested\":{\"kind\":\"x\"}}", *message, false, err));

  std::string json;
  CHECK(prp::message_to_json(*message, true, false, json, err));
  CHECK(json.find("\"seq\":\"42\"") != std::string::npos || json.find("\"seq\":42") != std::string::npos);
  CHECK(json.find("abc") != std::string::npos);

  CHECK(!prp::json_to_message("{\"seq\":\"not-a-number\"}", *message, false, err));
}

void test_json_escape() {
  CHECK_EQ(prp::json_escape("a\"b\\c\n\t"), std::string("a\\\"b\\\\c\\n\\t"));
  CHECK_EQ(prp::json_escape(std::string("\x01", 1)), std::string("\\u0001"));
  const std::string invalid("\xff", 1);
  CHECK_EQ(prp::json_escape(invalid), std::string("\\ufffd"));
}

void test_json_writer(const char* proto_path) {
  const auto dir = make_temp_dir("json");
  prp::Schema schema;
  std::string err;
  CHECK(schema.load(proto_path, {}, err));

  const auto* descriptor = schema.find_message("demo.TestRecord");
  google::protobuf::DynamicMessageFactory factory(schema.pool());
  std::unique_ptr<google::protobuf::Message> message(factory.GetPrototype(descriptor)->New());
  CHECK(prp::json_to_message("{\"seq\":7,\"name\":\"json-test\"}", *message, false, err));
  std::string serialized;
  CHECK(message->SerializeToString(&serialized));

  const std::string wire = prp::frame_message(serialized);

  const auto* out_descriptor = schema.find_message("demo.Nested");
  std::unique_ptr<google::protobuf::Message> out_message(
      factory.GetPrototype(out_descriptor)->New());
  CHECK(prp::json_to_message("{\"kind\":\"nested-out\",\"enabled\":true}", *out_message, false, err));
  std::string out_serialized;
  CHECK(out_message->SerializeToString(&out_serialized));
  const std::string out_wire = prp::frame_message(out_serialized);

  prp::JsonWriterConfig config;
  config.output_path = dir / "events.jsonl";
  config.schema = &schema;
  config.default_type_name = "demo.TestRecord";
  config.framing = prp::rec::Framing::VarintLength;
  prp::JsonWriter writer(config);
  CHECK(writer.start(err));

  writer.submit(prp::make_stream_open(1, prp::now_wall_ns(), "peer:1", "demo.TestRecord"));
  writer.submit(prp::make_data(1, prp::Direction::In, prp::now_wall_ns(),
                               std::vector<uint8_t>(wire.begin(), wire.end())));
  // Split a frame across chunks.
  writer.submit(prp::make_data(1, prp::Direction::In, prp::now_wall_ns(),
                               std::vector<uint8_t>(wire.begin(), wire.begin() + 1)));
  writer.submit(prp::make_data(1, prp::Direction::In, prp::now_wall_ns(),
                               std::vector<uint8_t>(wire.begin() + 1, wire.end())));
  // Outbound messages may use a different message type than the inbound stream.
  writer.submit(prp::make_data(1, prp::Direction::Out, prp::now_wall_ns(),
                               std::vector<uint8_t>(out_wire.begin(), out_wire.end()),
                               "demo.Nested"));
  writer.submit(prp::make_stream_close(1, prp::CloseReason::PeerClosed, prp::now_wall_ns()));
  writer.stop();

  const auto stats = writer.stats();
  CHECK(!stats.failed);
  CHECK(stats.messages >= 3);
  CHECK_EQ(stats.parse_errors, 0ull);

  std::string content;
  CHECK(prp::read_file(config.output_path, content, err));
  CHECK(content.find("\"event\":\"stream_open\"") != std::string::npos);
  CHECK(content.find("\"event\":\"stream_close\"") != std::string::npos);
  CHECK(content.find("json-test") != std::string::npos);
  CHECK(content.find("nested-out") != std::string::npos);
  CHECK(content.find("\"dir\":\"out\"") != std::string::npos);
  CHECK(content.find("\"type\":\"demo.Nested\"") != std::string::npos);
  CHECK(content.find("\"messages\"") == std::string::npos);

  std::filesystem::remove_all(dir);
}

void test_typed_json_writer(const char* proto_path) {
  const auto dir = make_temp_dir("json-typed");
  prp::Schema schema;
  std::string err;
  CHECK(schema.load(proto_path, {}, err));

  google::protobuf::DynamicMessageFactory factory(schema.pool());
  std::unique_ptr<google::protobuf::Message> record(
      factory.GetPrototype(schema.find_message("demo.TestRecord"))->New());
  CHECK(prp::json_to_message("{\"seq\":42,\"name\":\"typed\"}", *record, false, err));
  std::string record_bytes;
  CHECK(record->SerializeToString(&record_bytes));

  std::unique_ptr<google::protobuf::Message> nested(
      factory.GetPrototype(schema.find_message("demo.Nested"))->New());
  CHECK(prp::json_to_message("{\"kind\":\"nested-typed\"}", *nested, false, err));
  std::string nested_bytes;
  CHECK(nested->SerializeToString(&nested_bytes));

  const std::string wire = prp::frame_typed_message(1, record_bytes) +
                           prp::frame_typed_message(2, nested_bytes) +
                           prp::frame_typed_message(9, "unknown");

  prp::JsonWriterConfig config;
  config.output_path = dir / "events.jsonl";
  config.schema = &schema;
  config.framing = prp::rec::Framing::TypedLengthLE;
  config.type_map = {{1, "demo.TestRecord"}, {2, "demo.Nested"}};
  prp::JsonWriter writer(config);
  CHECK(writer.start(err));

  writer.submit(prp::make_stream_open(1, prp::now_wall_ns(), "peer:1", ""));
  writer.submit(prp::make_data(1, prp::Direction::In, prp::now_wall_ns(),
                               std::vector<uint8_t>(wire.begin(), wire.end())));
  writer.submit(prp::make_stream_close(1, prp::CloseReason::PeerClosed, prp::now_wall_ns()));
  writer.stop();

  const auto stats = writer.stats();
  CHECK(!stats.failed);
  CHECK_EQ(stats.messages, 2ull);
  CHECK_EQ(stats.unparsed, 1ull);
  CHECK_EQ(stats.parse_errors, 0ull);

  std::string content;
  CHECK(prp::read_file(config.output_path, content, err));
  CHECK(content.find("\"type\":\"demo.TestRecord\",\"type_id\":1") != std::string::npos);
  CHECK(content.find("\"type\":\"demo.Nested\",\"type_id\":2") != std::string::npos);
  CHECK(content.find("\"unparsed\":true") != std::string::npos);
  CHECK(content.find("\"type_id\":9") != std::string::npos);
  CHECK(content.find("typed") != std::string::npos);
  CHECK(content.find("nested-typed") != std::string::npos);

  std::filesystem::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: prp_tests <sample.proto>\n");
    return 2;
  }
  const char* proto = argv[1];
  // Homebrew's protobuf/absl shared libraries crash inside DescriptorPool
  // when the caller is ASan-instrumented, so sanitizer jobs run the
  // protobuf-independent subset. (See tests/CMakeLists.txt.)
  const char* skip_proto = std::getenv("PRP_SKIP_PROTO_TESTS");
  const bool with_proto = !(skip_proto && *skip_proto != '\0' && std::string(skip_proto) != "0");

  test_crc32c();
  test_sha256();
  test_base64();
  test_parsers();
  test_type_map_options();
  test_chunk_and_json_options();
  test_replay_options();
  test_passthrough_options();
  test_config_file();
  test_framer();
  test_item_queue();
  test_file_header();
  test_recorder_roundtrip();
  test_capture_reader();
  test_truncation_recovery();
  test_json_escape();
  if (with_proto) {
    test_schema(proto);
    test_json_writer(proto);
    test_typed_json_writer(proto);
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
