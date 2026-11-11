#include "prp/replay/replay_app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include "prp/net/client.h"
#include "prp/proto/schema.h"
#include "prp/record/reader.h"
#include "prp/util/log.h"
#include "prp/util/text.h"

namespace prp {
namespace {

namespace asio = boost::asio;
using ErrorCode = boost::system::error_code;

constexpr size_t kTotalHighWater = 64ull * 1024 * 1024;
constexpr size_t kStreamQueueLimit = 256ull * 1024 * 1024;
constexpr auto kBackpressureRetry = std::chrono::milliseconds(5);
constexpr auto kFinishTimeout = std::chrono::seconds(30);
constexpr unsigned kMaxReconnects = 10;
constexpr int64_t kProgressIntervalNs = 100'000'000;  // refresh at most every 100 ms

bool selected_direction(const Options& options, Direction dir) {
  switch (options.replay_direction) {
    case ReplayDirection::In: return dir == Direction::In;
    case ReplayDirection::Out: return dir == Direction::Out;
    case ReplayDirection::Both: return true;
  }
  return false;
}

std::string direction_name(ReplayDirection dir) {
  switch (dir) {
    case ReplayDirection::In: return "in";
    case ReplayDirection::Out: return "out";
    case ReplayDirection::Both: return "both";
  }
  return "unknown";
}

int64_t scaled_delay_ns(int64_t gap_ns, const Options& options) {
  if (gap_ns <= 0 || options.replay_speed <= 0.0) return 0;
  const double scaled = static_cast<double>(gap_ns) / options.replay_speed;
  double capped = scaled;
  if (options.max_gap.count() > 0) {
    const double cap = static_cast<double>(options.max_gap.count()) * 1'000'000.0;
    if (capped > cap) capped = cap;
  }
  const double kMax = static_cast<double>(std::numeric_limits<int64_t>::max() / 2);
  if (!std::isfinite(capped) || capped >= kMax) return static_cast<int64_t>(kMax);
  if (capped <= 0.0) return 0;
  return static_cast<int64_t>(capped);
}

class ReplayRunner {
 public:
  using Strand = asio::strand<asio::io_context::executor_type>;
  using FinishCallback = std::function<void()>;

  ReplayRunner(const Options& options, asio::io_context& io, FinishCallback finish)
      : options_(options),
        strand_(asio::make_strand(io)),
        timer_(strand_),
        watchdog_(strand_),
        finish_(std::move(finish)) {}

  ReplayRunner(const ReplayRunner&) = delete;
  ReplayRunner& operator=(const ReplayRunner&) = delete;

  Strand& strand() { return strand_; }
  const std::string& failure() const { return failure_; }
  int exit_code() const { return exit_code_; }

  void start() {
    asio::post(strand_, [this] { prepare(); });
  }

  void request_stop(int signal_number) {
    if (completed_) return;
    log_printf(LogLevel::Info, "received signal %d, aborting replay", signal_number);
    for (auto& client : live_clients_) client->abort();
    live_clients_.clear();
    stream_clients_.clear();
    complete(0, "interrupted");
  }

 private:
  struct StreamMeta {
    std::string peer;
    std::string type_name;
  };

  void prepare() {
    if (completed_) return;
    started_ns_ = now_monotonic_ns();

    if (!options_.proto_file.empty()) {
      if (!schema_.load(options_.proto_file, options_.proto_paths, failure_)) {
        complete(2, "schema error: " + failure_);
        return;
      }
      schema_loaded_ = true;
    }

    std::string err;
    if (!options_.input_file.empty()) {
      files_.push_back(options_.input_file);
    } else if (!list_capture_files(options_.input_dir, files_, err)) {
      complete(2, err);
      return;
    }
    if (files_.empty()) {
      complete(2, options_.input_dir.string() + " contains no capture-*.pcr files");
      return;
    }

    log_printf(LogLevel::Info, "replaying %zu capture chunk(s) to %s:%u (speed=%s, direction=%s)",
               files_.size(), options_.target_host.c_str(),
               static_cast<unsigned>(options_.target_port),
               options_.replay_speed == 0.0 ? "max" : std::to_string(options_.replay_speed).c_str(),
               direction_name(options_.replay_direction).c_str());

    if (!open_next_file()) return;
    load_next_record();
  }

  std::string progress_prefix() const {
    if (files_.size() <= 1) return {};
    return "[" + std::to_string(file_index_ + 1) + "/" + std::to_string(files_.size()) + "] ";
  }

  // Refreshes the one-line progress display for the current capture file.
  // `force` bypasses the throttle (used for the first and last update).
  void update_progress(bool force = false) {
    if (!reader_.is_open()) return;
    const uint64_t size = reader_.file_size();
    unsigned percent = 100;
    if (size > 0) {
      const uint64_t consumed = std::min(reader_.bytes_consumed(), size);
      percent = static_cast<unsigned>(consumed * 100 / size);
      if (percent > 100) percent = 100;
    }
    const int64_t now = now_monotonic_ns();
    if (!force) {
      if (static_cast<int>(percent) == last_progress_percent_) return;
      if (now - last_progress_ns_ < kProgressIntervalNs) return;
    }
    last_progress_percent_ = static_cast<int>(percent);
    last_progress_ns_ = now;
    char tail[16];
    std::snprintf(tail, sizeof(tail), "%3u%%", percent);
    set_progress_line(progress_prefix() + reader_.path().string() + " " + tail);
  }

  void begin_progress() {
    last_progress_percent_ = -1;
    last_progress_ns_ = 0;
    update_progress(true);
  }

  void finish_progress() {
    if (reader_.is_open()) update_progress(true);
    clear_progress_line();
  }

  bool open_next_file() {
    while (file_index_ < files_.size()) {
      const fs::path& path = files_[file_index_];
      std::string err;
      if (!reader_.open(path, err)) {
        if (options_.strict) {
          complete(1, err);
          return false;
        }
        log_printf(LogLevel::Warn, "skipping unreadable capture %s: %s", path.string().c_str(),
                   err.c_str());
        ++file_index_;
        continue;
      }

      const rec::FileHeader& header = reader_.header();
      if (!have_session_) {
        session_uuid_ = header.session_uuid;
        session_framing_ = header.framing;
        have_session_ = true;
        if (!validate_schema(header, path)) return false;
      } else {
        if (header.session_uuid != session_uuid_) {
          const std::string message = "capture " + path.string() +
                                      " belongs to a different session (uuid mismatch)";
          if (options_.strict) {
            complete(1, message);
            return false;
          }
          log_printf(LogLevel::Warn, "%s", message.c_str());
        }
        if (header.framing != session_framing_) {
          log_printf(LogLevel::Warn, "capture %s uses framing=%s while the session started with %s",
                     path.string().c_str(), rec::framing_name(header.framing),
                     rec::framing_name(session_framing_));
        }
        if (!validate_schema(header, path)) return false;
      }

      log_printf(LogLevel::Debug, "opened capture %s (%llu record(s) so far)",
                 path.string().c_str(), static_cast<unsigned long long>(records_read_));
      begin_progress();
      file_has_trailer_ = false;
      ++valid_files_;
      return true;
    }

    if (valid_files_ == 0) {
      complete(1, "no readable capture files found");
      return false;
    }
    begin_finish();
    return false;
  }

  bool validate_schema(const rec::FileHeader& header, const fs::path& path) {
    if (!schema_loaded_) return true;
    if (header.proto_sha256 == schema_.digest()) return true;
    const std::string message =
        "capture " + path.string() + " was recorded with a different schema (capture " +
        Sha256::hex(header.proto_sha256) + ", --proto " + Sha256::hex(schema_.digest()) + ")";
    if (options_.strict) {
      complete(1, message);
      return false;
    }
    if (!schema_mismatch_warned_) {
      log_printf(LogLevel::Warn, "%s", message.c_str());
      schema_mismatch_warned_ = true;
    }
    return true;
  }

  void load_next_record() {
    if (completed_ || finishing_) return;
    if (!reader_.is_open() && !open_next_file()) return;

    while (!completed_) {
      CaptureRecord record;
      std::string detail;
      const ReadStatus status = reader_.next(record, detail);
      if (status == ReadStatus::Record) {
        ++records_read_;
        update_progress();
        if (record.type == rec::RecordType::Trailer) file_has_trailer_ = true;
        current_ = std::move(record);
        have_current_ = true;
        compute_schedule();
        schedule_current();
        return;
      }
      if (status == ReadStatus::Eof) {
        finish_progress();
        const std::string path = reader_.path().string();
        if (!file_has_trailer_) {
          const std::string message =
              path + ": capture has no trailer (interrupted recording)";
          if (options_.strict) {
            reader_.close();
            complete(1, message);
            return;
          }
          log_printf(LogLevel::Warn, "%s (replaying the valid prefix)", message.c_str());
          ++damaged_files_;
        }
        reader_.close();
        ++file_index_;
        if (!open_next_file()) return;
        continue;
      }

      finish_progress();
      const std::string message =
          reader_.path().string() + ": damaged capture at record " +
          std::to_string(reader_.records_read()) + ": " + detail;
      reader_.close();
      if (options_.strict) {
        complete(1, message);
        return;
      }
      log_printf(LogLevel::Warn, "%s (replaying the valid prefix)", message.c_str());
      ++damaged_files_;
      ++file_index_;
      if (!open_next_file()) return;
    }
  }

  void compute_schedule() {
    const int64_t now = now_monotonic_ns();
    if (!have_timeline_) {
      have_timeline_ = true;
      last_ts_ns_ = current_.ts_ns;
      timeline_ns_ = now;
      return;
    }
    int64_t gap = current_.ts_ns - last_ts_ns_;
    last_ts_ns_ = current_.ts_ns;
    if (gap < 0) gap = 0;
    timeline_ns_ += scaled_delay_ns(gap, options_);
  }

  bool backpressured() const {
    size_t total = 0;
    for (const auto& client : live_clients_) total += client->queued_bytes();
    if (total >= kTotalHighWater) return true;
    if (current_.type == rec::RecordType::Data && selected_direction(options_, current_.dir)) {
      const auto it = stream_clients_.find(current_.stream_id);
      if (it != stream_clients_.end() &&
          it->second->queued_bytes() + current_.data.size() >= kStreamQueueLimit) {
        return true;
      }
    }
    return false;
  }

  void schedule_current() {
    if (completed_ || !have_current_) return;
    if (backpressured()) {
      // Do not read ahead while the network cannot keep up. Resync the
      // timeline afterwards so the backlog is not replayed as a burst.
      timeline_ns_ = std::max(timeline_ns_, now_monotonic_ns());
      arm_timer(kBackpressureRetry, [this] { schedule_current(); });
      return;
    }

    const int64_t now = now_monotonic_ns();
    if (timeline_ns_ > now) {
      arm_timer(std::chrono::nanoseconds(timeline_ns_ - now), [this] { dispatch_current(); });
    } else {
      asio::post(strand_, [this] { dispatch_current(); });
    }
  }

  template <typename Rep, typename Period, typename Handler>
  void arm_timer(std::chrono::duration<Rep, Period> delay, Handler handler) {
    timer_.expires_after(delay);
    timer_.async_wait(asio::bind_executor(strand_, [handler = std::move(handler)](const ErrorCode& ec) {
      if (!ec) handler();
    }));
  }

  void dispatch_current() {
    if (completed_ || !have_current_) return;
    const int64_t now = now_monotonic_ns();

    switch (current_.type) {
      case rec::RecordType::StreamOpen: {
        StreamMeta& meta = streams_[current_.stream_id];
        if (meta.peer.empty()) meta.peer = current_.peer;
        if (meta.type_name.empty()) meta.type_name = current_.type_name;
        break;
      }
      case rec::RecordType::Data: {
        if (selected_direction(options_, current_.dir)) {
          const uint32_t id = current_.stream_id;
          auto it = stream_clients_.find(id);
          std::shared_ptr<ClientConnection> client;
          if (it != stream_clients_.end() && !it->second->closed() && !it->second->closing()) {
            client = it->second;
          } else {
            client = make_client(id);
            stream_clients_[id] = client;
          }
          const size_t size = current_.data.size();
          ++data_records_;
          bytes_replayed_ += size;
          client->write(std::move(current_.data));
        } else {
          ++skipped_data_;
        }
        break;
      }
      case rec::RecordType::StreamClose: {
        const auto it = stream_clients_.find(current_.stream_id);
        if (it != stream_clients_.end()) it->second->close_after_flush();
        break;
      }
      case rec::RecordType::Marker:
      case rec::RecordType::Trailer:
        break;
    }

    // If we fell behind (backpressure, slow disk, timer jitter), start the
    // next gap from now instead of accumulating a growing burst.
    if (now > timeline_ns_) timeline_ns_ = now;
    have_current_ = false;
    current_ = CaptureRecord{};
    load_next_record();
  }

  std::shared_ptr<ClientConnection> make_client(uint32_t stream_id) {
    ClientConfig config;
    config.host = options_.target_host;
    config.port = options_.target_port;
    config.connect_timeout = options_.connect_timeout;
    config.retry_interval = options_.retry_interval;
    config.connect_retries = options_.connect_retries;
    config.max_queue_bytes = kStreamQueueLimit;

    auto client = std::make_shared<ClientConnection>(strand_, config);
    live_clients_.insert(client);
    ++streams_created_;

    std::weak_ptr<ClientConnection> weak = client;
    client->set_ready_callback([this, stream_id] {
      if (completed_) return;
      ++stream_connects_;
      log_printf(LogLevel::Debug, "stream %u connected", stream_id);
    });
    client->set_closed_callback([this, stream_id, weak] {
      auto locked = weak.lock();
      if (!locked) return;
      live_clients_.erase(locked);
      const auto it = stream_clients_.find(stream_id);
      if (it != stream_clients_.end() && it->second == locked) stream_clients_.erase(it);
      if (finishing_ && live_clients_.empty()) complete_success();
    });
    client->set_error_callback(
        [this, stream_id, weak](const std::string& message) { on_client_error(stream_id, weak, message); });
    client->start();
    return client;
  }

  void on_client_error(uint32_t stream_id, const std::weak_ptr<ClientConnection>& weak,
                       const std::string& message) {
    if (completed_) return;
    auto client = weak.lock();
    if (!client) return;

    const auto it = stream_clients_.find(stream_id);
    const bool current = it != stream_clients_.end() && it->second == client;
    if (!current) {
      // A retired connection failed while flushing. There is no stream to
      // resume, so treat it as a normal replay failure.
      fail("stream " + std::to_string(stream_id) + ": " + message);
      return;
    }

    const unsigned reconnects = reconnect_counts_[stream_id];
    if (options_.reconnect && client->connects() > 0 && reconnects < kMaxReconnects) {
      reconnect_counts_[stream_id] = reconnects + 1;
      log_printf(LogLevel::Warn, "stream %u lost connection (%s), reconnecting (%u/%u)", stream_id,
                 message.c_str(), reconnects + 1, kMaxReconnects);
      client->restart();
      return;
    }
    fail("stream " + std::to_string(stream_id) + ": " + message);
  }

  void begin_finish() {
    if (completed_ || finishing_) return;
    finishing_ = true;
    if (live_clients_.empty()) {
      complete_success();
      return;
    }
    for (auto& client : live_clients_) client->close_after_flush();
    watchdog_.expires_after(kFinishTimeout);
    watchdog_.async_wait(asio::bind_executor(strand_, [this](const ErrorCode& ec) {
      if (ec || completed_) return;
      fail("timed out waiting for pending writes to drain");
    }));
  }

  void complete_success() {
    if (completed_) return;
    const int64_t elapsed = now_monotonic_ns() - started_ns_;
    log_printf(LogLevel::Info,
               "replay finished: %llu records, %llu data records, %s sent, %llu stream(s), "
               "%llu connection(s), %llu skipped, %llu damaged file(s), %.3fs",
               static_cast<unsigned long long>(records_read_),
               static_cast<unsigned long long>(data_records_),
               format_bytes(bytes_replayed_).c_str(),
               static_cast<unsigned long long>(streams_created_),
               static_cast<unsigned long long>(stream_connects_),
               static_cast<unsigned long long>(skipped_data_),
               static_cast<unsigned long long>(damaged_files_), static_cast<double>(elapsed) / 1e9);
    complete(0, {});
  }

  void fail(std::string message) {
    if (completed_) return;
    failure_ = std::move(message);
    complete(1, failure_);
  }

  void complete(int code, std::string failure) {
    if (completed_) return;
    clear_progress_line();
    completed_ = true;
    exit_code_ = code;
    if (!failure.empty() && failure_.empty()) failure_ = std::move(failure);
    timer_.cancel();
    watchdog_.cancel();
    for (auto& client : live_clients_) client->abort();
    live_clients_.clear();
    stream_clients_.clear();
    if (finish_) finish_();
  }

  const Options& options_;
  Strand strand_;
  asio::steady_timer timer_;
  asio::steady_timer watchdog_;
  FinishCallback finish_;

  Schema schema_;
  bool schema_loaded_ = false;
  bool schema_mismatch_warned_ = false;

  std::vector<fs::path> files_;
  size_t file_index_ = 0;
  size_t valid_files_ = 0;
  CaptureReader reader_;
  bool file_has_trailer_ = false;
  bool have_session_ = false;
  std::array<uint8_t, 16> session_uuid_{};
  rec::Framing session_framing_ = rec::Framing::VarintLength;

  bool have_current_ = false;
  CaptureRecord current_;
  bool have_timeline_ = false;
  int64_t last_ts_ns_ = 0;
  int64_t timeline_ns_ = 0;

  int last_progress_percent_ = -1;
  int64_t last_progress_ns_ = 0;

  std::map<uint32_t, StreamMeta> streams_;
  std::map<uint32_t, std::shared_ptr<ClientConnection>> stream_clients_;
  std::set<std::shared_ptr<ClientConnection>> live_clients_;
  std::map<uint32_t, unsigned> reconnect_counts_;

  bool finishing_ = false;
  bool completed_ = false;
  int exit_code_ = 0;
  std::string failure_;
  int64_t started_ns_ = 0;

  uint64_t records_read_ = 0;
  uint64_t data_records_ = 0;
  uint64_t skipped_data_ = 0;
  uint64_t bytes_replayed_ = 0;
  uint64_t stream_connects_ = 0;
  uint64_t streams_created_ = 0;
  uint64_t damaged_files_ = 0;
};

}  // namespace

int run_replay(const Options& options, std::string& err) {
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  bool finished = false;
  auto runner = std::make_shared<ReplayRunner>(options, io, [&work, &io, &finished] {
    finished = true;
    work.reset();
    io.stop();
  });

  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait(asio::bind_executor(runner->strand(), [runner](const ErrorCode& ec, int signal_number) {
    if (!ec) runner->request_stop(signal_number);
  }));

  runner->start();
  std::vector<std::thread> io_threads;
  for (unsigned i = 1; i < options.io_threads; ++i) {
    io_threads.emplace_back([&io] { io.run(); });
  }
  io.run();
  for (auto& thread : io_threads) thread.join();

  if (!finished) {
    err = "replay loop terminated unexpectedly";
    return 1;
  }
  err = runner->failure();
  return runner->exit_code();
}

}  // namespace prp
