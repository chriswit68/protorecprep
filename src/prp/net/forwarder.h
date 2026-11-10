#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>

#include "prp/net/client.h"

namespace prp {

struct ForwardConfig {
  bool enabled = false;
  std::string host;
  uint16_t port = 0;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds retry_interval{1000};
  uint64_t connect_retries = 5;
  bool reconnect = false;
  size_t queue_limit = 16ull * 1024 * 1024;
  unsigned max_reconnects = 10;
};

// Forwards inbound client bytes to a target server over one lazily opened TCP
// connection per source stream, and (optionally) forwards raw target responses
// back to the client. Recording and forwarding are independent: a forwarding
// failure never closes the source stream.
//
// Thread-safety: the constructor takes the executor that runs the owning
// session's strand. All public methods must be called from that executor, and
// all callbacks are delivered on it.
class SessionForwarder : public std::enable_shared_from_this<SessionForwarder> {
 public:
  using Callback = std::function<void()>;
  using ErrorCallback = std::function<void(const std::string&)>;
  using DataCallback = std::function<void(std::vector<uint8_t>)>;

  SessionForwarder(boost::asio::any_io_executor executor, ForwardConfig config);

  SessionForwarder(const SessionForwarder&) = delete;
  SessionForwarder& operator=(const SessionForwarder&) = delete;

  void set_drained_callback(Callback callback) { drained_cb_ = std::move(callback); }
  void set_closed_callback(Callback callback) { closed_cb_ = std::move(callback); }
  void set_error_callback(ErrorCallback callback) { error_cb_ = std::move(callback); }
  // Optional reverse direction: raw bytes received from the target.
  void set_response_callback(DataCallback callback) { response_cb_ = std::move(callback); }

  // Opens the target connection lazily on the first forwarded byte.
  void forward(std::vector<uint8_t> bytes);
  // Flushes queued bytes, then closes the target connection.
  void close();
  // Drops the connection and any queued bytes immediately.
  void abort();

  // Flow control for the target -> client direction.
  void pause_responses();
  void resume_responses();

  bool started() const { return started_; }
  bool failed() const { return failed_; }
  bool closed() const { return closed_; }
  bool active() const { return started_ && !failed_ && !closed_; }
  bool responses_paused() const;
  size_t queued_bytes() const;
  uint64_t bytes_forwarded() const;
  uint64_t bytes_received() const;
  uint64_t connects() const;

 private:
  void ensure_started();
  void handle_data(std::vector<uint8_t> bytes);
  void handle_drained();
  void handle_closed();
  void handle_error(const std::string& message);
  void invoke(Callback& callback);
  void invoke_error(const std::string& message);

  ForwardConfig config_;
  boost::asio::any_io_executor executor_;
  std::shared_ptr<ClientConnection> client_;
  ErrorCallback error_cb_;
  Callback drained_cb_;
  Callback closed_cb_;
  DataCallback response_cb_;
  unsigned reconnects_ = 0;
  bool started_ = false;
  bool failed_ = false;
  bool closed_ = false;
};

}  // namespace prp
