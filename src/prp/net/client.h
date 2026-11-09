#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

namespace prp {

struct ClientConfig {
  std::string host;
  uint16_t port = 0;
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds retry_interval{1000};
  // Total connect attempts (including the first one).
  uint64_t connect_retries = 5;
  // Soft bound for queued bytes. The replay scheduler applies backpressure
  // before this limit is reached, so it normally never trips.
  size_t max_queue_bytes = 256ull * 1024 * 1024;
};

// Asynchronous TCP client used by the replay mode.
//
// Thread-safety: the constructor takes the executor that runs the replay
// scheduler (a strand). All public methods must be called from that executor,
// and all callbacks are posted back to it. This makes the client safe to use
// with multiple io_context threads without additional locking.
class ClientConnection : public std::enable_shared_from_this<ClientConnection> {
 public:
  using Callback = std::function<void()>;
  using ErrorCallback = std::function<void(const std::string&)>;

  ClientConnection(boost::asio::any_io_executor executor, ClientConfig config);

  ClientConnection(const ClientConnection&) = delete;
  ClientConnection& operator=(const ClientConnection&) = delete;

  void start();
  // Reconnects while keeping unsent data in the queue. Used by --reconnect.
  void restart();

  void write(std::vector<uint8_t> bytes);
  // Closes the socket after all queued bytes have been written.
  void close_after_flush();
  // Drops the connection and any queued data immediately.
  void abort();

  void set_ready_callback(Callback callback) { ready_cb_ = std::move(callback); }
  void set_drained_callback(Callback callback) { drained_cb_ = std::move(callback); }
  void set_closed_callback(Callback callback) { closed_cb_ = std::move(callback); }
  void set_error_callback(ErrorCallback callback) { error_cb_ = std::move(callback); }

  bool started() const { return started_; }
  bool connected() const { return connected_; }
  bool closing() const { return close_requested_; }
  bool closed() const { return closed_; }
  bool failed() const { return failed_; }
  size_t queued_bytes() const { return queued_bytes_; }
  uint64_t bytes_written() const { return bytes_written_; }
  uint64_t connects() const { return connects_; }

 private:
  void do_connect();
  void start_attempt_timeout(uint64_t attempt_id);
  void on_connect_timeout(const boost::system::error_code& ec, uint64_t attempt_id);
  void fail_attempt(uint64_t attempt_id, const std::string& message);
  void schedule_retry();
  void do_write();
  void on_write(const boost::system::error_code& ec, size_t transferred);
  void finish_close();
  void notify(Callback& callback);
  void notify_error(const std::string& message);

  ClientConfig config_;
  boost::asio::any_io_executor executor_;
  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::ip::tcp::socket socket_;
  boost::asio::steady_timer connect_timer_;
  boost::asio::steady_timer retry_timer_;

  std::deque<std::vector<uint8_t>> queue_;
  size_t front_offset_ = 0;
  size_t queued_bytes_ = 0;

  Callback ready_cb_;
  Callback drained_cb_;
  Callback closed_cb_;
  ErrorCallback error_cb_;

  uint64_t attempt_id_ = 0;
  uint64_t attempts_left_ = 0;
  uint64_t connects_ = 0;
  uint64_t bytes_written_ = 0;
  bool started_ = false;
  bool connected_ = false;
  bool writing_ = false;
  bool close_requested_ = false;
  bool closed_ = false;
  bool failed_ = false;
};

}  // namespace prp
