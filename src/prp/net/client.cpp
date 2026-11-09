#include "prp/net/client.h"

#include <algorithm>
#include <utility>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

#include "prp/util/log.h"

namespace prp {
namespace asio = boost::asio;
using asio::ip::tcp;
using ErrorCode = boost::system::error_code;

ClientConnection::ClientConnection(asio::any_io_executor executor, ClientConfig config)
    : config_(std::move(config)),
      executor_(std::move(executor)),
      resolver_(executor_),
      socket_(executor_),
      connect_timer_(executor_),
      retry_timer_(executor_) {}

void ClientConnection::start() {
  if (started_ || closed_) return;
  started_ = true;
  failed_ = false;
  attempts_left_ = config_.connect_retries;
  do_connect();
}

void ClientConnection::restart() {
  // Restart is only meaningful for a connection that broke after it was
  // established. A cleanly closed connection must be replaced by the caller.
  if (!started_ || closed_ || connected_) return;
  failed_ = false;
  ++attempt_id_;  // invalidate callbacks from the previous attempt
  ErrorCode ignored;
  connect_timer_.cancel();
  retry_timer_.cancel();
  socket_.close(ignored);
  attempts_left_ = config_.connect_retries;
  do_connect();
}

void ClientConnection::write(std::vector<uint8_t> bytes) {
  if (closed_ || bytes.empty()) return;
  queued_bytes_ += bytes.size();
  queue_.push_back(std::move(bytes));
  if (connected_) do_write();
}

void ClientConnection::close_after_flush() {
  if (closed_) return;
  close_requested_ = true;
  if (writing_) return;
  if (queue_.empty()) {
    finish_close();
    return;
  }
  // Still connecting: keep the attempt alive so the queued bytes can drain.
  if (connected_) do_write();
}

void ClientConnection::abort() {
  if (closed_) return;
  close_requested_ = true;
  ++attempt_id_;
  ErrorCode ignored;
  connect_timer_.cancel();
  retry_timer_.cancel();
  socket_.close(ignored);
  connected_ = false;
  writing_ = false;
  queue_.clear();
  front_offset_ = 0;
  queued_bytes_ = 0;
  finish_close();
}

void ClientConnection::do_connect() {
  if (closed_ || connected_) return;
  if (attempts_left_ == 0) {
    failed_ = true;
    notify_error("cannot connect to " + config_.host + ":" + std::to_string(config_.port) +
                 " after " + std::to_string(config_.connect_retries) + " attempt(s)");
    return;
  }
  --attempts_left_;
  const uint64_t attempt_id = ++attempt_id_;
  ErrorCode ignored;
  socket_.close(ignored);
  connected_ = false;

  auto self = shared_from_this();
  if (config_.connect_timeout.count() > 0) {
    connect_timer_.expires_after(config_.connect_timeout);
    connect_timer_.async_wait(asio::bind_executor(executor_, [self, attempt_id](const ErrorCode& ec) {
                               if (ec) return;
                               if (self->closed_ || attempt_id != self->attempt_id_) return;
                               self->fail_attempt(attempt_id, "connect timeout");
                             }));
  }

  const std::string port = std::to_string(config_.port);
  resolver_.async_resolve(
      config_.host, port,
      asio::bind_executor(executor_, [self, attempt_id](const ErrorCode& ec,
                                                       const tcp::resolver::results_type& results) {
        if (self->closed_ || attempt_id != self->attempt_id_) return;
        if (ec) {
          self->fail_attempt(attempt_id, "resolve failed: " + ec.message());
          return;
        }
        asio::async_connect(
            self->socket_, results,
            asio::bind_executor(self->executor_, [self, attempt_id](const ErrorCode& connect_ec,
                                                                   const tcp::endpoint&) {
              if (self->closed_ || attempt_id != self->attempt_id_) return;
              if (connect_ec) {
                self->fail_attempt(attempt_id, "connect failed: " + connect_ec.message());
                return;
              }
              self->connect_timer_.cancel();
              self->connected_ = true;
              self->failed_ = false;
              ++self->connects_;
              ErrorCode opt_ec;
              self->socket_.set_option(tcp::no_delay(true), opt_ec);
              self->socket_.set_option(asio::socket_base::keep_alive(true), opt_ec);
              log_printf(LogLevel::Debug, "connected to %s:%u", self->config_.host.c_str(),
                         static_cast<unsigned>(self->config_.port));
              self->notify(self->ready_cb_);
              self->do_write();
            }));
      }));
}

void ClientConnection::fail_attempt(uint64_t attempt_id, const std::string& message) {
  if (closed_ || attempt_id != attempt_id_) return;
  ++attempt_id_;  // invalidate any callbacks still in flight for this attempt
  connect_timer_.cancel();
  ErrorCode ignored;
  socket_.close(ignored);
  connected_ = false;

  if (attempts_left_ == 0) {
    failed_ = true;
    notify_error("cannot connect to " + config_.host + ":" + std::to_string(config_.port) + ": " +
                 message + " (gave up after " + std::to_string(config_.connect_retries) +
                 " attempt(s))");
    return;
  }

  log_printf(LogLevel::Debug, "connect to %s:%u failed (%s), %llu attempt(s) left",
             config_.host.c_str(), static_cast<unsigned>(config_.port), message.c_str(),
             static_cast<unsigned long long>(attempts_left_));
  auto self = shared_from_this();
  if (config_.retry_interval.count() <= 0) {
    asio::post(executor_, [self] { self->do_connect(); });
    return;
  }
  retry_timer_.expires_after(config_.retry_interval);
  retry_timer_.async_wait(asio::bind_executor(executor_, [self](const ErrorCode& ec) {
    if (ec || self->closed_) return;
    self->do_connect();
  }));
}

void ClientConnection::do_write() {
  if (closed_ || !connected_ || writing_ || queue_.empty()) return;
  writing_ = true;
  auto self = shared_from_this();
  const std::vector<uint8_t>& front = queue_.front();
  asio::async_write(socket_, asio::buffer(front.data() + front_offset_, front.size() - front_offset_),
                    asio::bind_executor(executor_, [self](const ErrorCode& ec, size_t transferred) {
                      self->on_write(ec, transferred);
                    }));
}

void ClientConnection::on_write(const ErrorCode& ec, size_t transferred) {
  writing_ = false;
  if (closed_) return;

  if (!queue_.empty()) {
    const size_t remaining = queue_.front().size() - front_offset_;
    const size_t used = std::min(transferred, remaining);
    bytes_written_ += used;
    queued_bytes_ = queued_bytes_ > used ? queued_bytes_ - used : 0;
    front_offset_ += used;
    if (front_offset_ >= queue_.front().size()) {
      queue_.pop_front();
      front_offset_ = 0;
    }
  }

  if (ec) {
    connected_ = false;
    failed_ = true;
    ErrorCode ignored;
    socket_.close(ignored);
    notify_error("write to " + config_.host + ":" + std::to_string(config_.port) +
                 " failed: " + ec.message());
    return;
  }

  if (queue_.empty()) {
    notify(drained_cb_);
    if (close_requested_) finish_close();
    return;
  }
  do_write();
}

void ClientConnection::finish_close() {
  if (closed_) return;
  closed_ = true;
  connected_ = false;
  writing_ = false;
  ++attempt_id_;  // ignore late resolve/connect/timeout callbacks
  ErrorCode ignored;
  connect_timer_.cancel();
  retry_timer_.cancel();
  socket_.shutdown(tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
  log_printf(LogLevel::Debug, "connection to %s:%u closed", config_.host.c_str(),
             static_cast<unsigned>(config_.port));
  notify(closed_cb_);
}

void ClientConnection::notify(Callback& callback) {
  if (!callback) return;
  auto fn = callback;
  asio::post(executor_, [fn = std::move(fn)] { fn(); });
}

void ClientConnection::notify_error(const std::string& message) {
  if (!error_cb_) return;
  auto fn = error_cb_;
  asio::post(executor_, [fn = std::move(fn), message] { fn(message); });
}

}  // namespace prp
