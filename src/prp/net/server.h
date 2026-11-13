#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "prp/lib.h"
#include "prp/net/forwarder.h"

namespace prp {

struct SendSpec {
  bool enabled = false;
  std::string type_name;
  std::string json_content;
  std::string framed_bytes;  // serialized message incl. the active framing header
  std::chrono::milliseconds once_after{0};
  bool has_once_after = false;
  std::chrono::milliseconds every{0};
  bool has_every = false;
  uint64_t count = 0;  // 0 = unlimited
};

struct ServerConfig {
  std::string listen_host = "0.0.0.0";
  uint16_t listen_port = 5000;
  rec::Framing framing = rec::Framing::VarintLength;
  std::string default_type_name;
  size_t recv_buffer = 256 * 1024;
  size_t max_frame = 16 * 1024 * 1024;
  size_t max_connections = 1024;
  std::chrono::milliseconds idle_timeout{0};
  SendSpec send;
  ForwardConfig forward;
};

struct ForwardStats {
  uint64_t streams = 0;         // streams that opened a target connection
  uint64_t bytes = 0;           // bytes written to target servers
  uint64_t response_bytes = 0;  // bytes received from target servers
  uint64_t connects = 0;        // successful target connections (incl. reconnects)
  uint64_t failures = 0;        // streams whose forwarding stopped on error
};

class Session;

class Server {
 public:
  Server(boost::asio::io_context& io, ServerConfig config, Recorder& recorder);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  bool open(std::string& err);
  void start();

  // Initiates graceful shutdown: stops accepting and closes all sessions. The
  // `stopped` callback fires exactly once, when the last session is gone.
  void shutdown();
  void set_stopped_callback(std::function<void()> callback);

  bool stopping() const { return stopping_.load(std::memory_order_acquire); }
  uint64_t total_accepted() const { return total_accepted_.load(std::memory_order_relaxed); }
  size_t active_sessions() const;

  const ServerConfig& config() const { return config_; }
  Recorder& recorder() { return recorder_; }

  // Aggregated forwarding statistics (passthrough mode).
  void note_forward_result(bool started, uint64_t bytes, uint64_t connects, bool failed,
                           uint64_t response_bytes);
  ForwardStats forward_stats() const;

  void on_session_closed(uint32_t id);

 private:
  void do_accept();
  void fire_stopped();

  boost::asio::io_context& io_;
  ServerConfig config_;
  Recorder& recorder_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stopped_fired_{false};
  std::atomic<uint64_t> total_accepted_{0};
  std::atomic<uint64_t> forward_streams_{0};
  std::atomic<uint64_t> forward_bytes_{0};
  std::atomic<uint64_t> forward_response_bytes_{0};
  std::atomic<uint64_t> forward_connects_{0};
  std::atomic<uint64_t> forward_failures_{0};

  mutable std::mutex mutex_;
  std::map<uint32_t, std::shared_ptr<Session>> sessions_;
  uint32_t next_session_id_ = 1;
  std::function<void()> stopped_callback_;
};

}  // namespace prp
