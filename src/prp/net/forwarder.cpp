#include "prp/net/forwarder.h"

#include <utility>

#include <boost/asio/post.hpp>

#include "prp/util/log.h"

namespace prp {
namespace asio = boost::asio;

SessionForwarder::SessionForwarder(asio::any_io_executor executor, ForwardConfig config)
    : config_(std::move(config)), executor_(std::move(executor)) {}

void SessionForwarder::ensure_started() {
  if (started_ || failed_ || closed_) return;
  started_ = true;

  ClientConfig client_config;
  client_config.host = config_.host;
  client_config.port = config_.port;
  client_config.connect_timeout = config_.connect_timeout;
  client_config.retry_interval = config_.retry_interval;
  client_config.connect_retries = config_.connect_retries;
  client_config.max_queue_bytes = config_.queue_limit;

  client_ = std::make_shared<ClientConnection>(executor_, client_config);
  std::weak_ptr<SessionForwarder> weak = shared_from_this();
  client_->set_drained_callback([weak] {
    if (auto self = weak.lock()) self->handle_drained();
  });
  client_->set_closed_callback([weak] {
    if (auto self = weak.lock()) self->handle_closed();
  });
  client_->set_error_callback([weak](const std::string& message) {
    if (auto self = weak.lock()) self->handle_error(message);
  });
  client_->set_data_callback([weak](std::vector<uint8_t> bytes) {
    if (auto self = weak.lock()) self->handle_data(std::move(bytes));
  });
  client_->start();
  client_->start_reading();
}

void SessionForwarder::forward(std::vector<uint8_t> bytes) {
  if (closed_ || failed_) return;
  ensure_started();
  if (!client_) return;
  client_->write(std::move(bytes));
}

void SessionForwarder::close() {
  if (closed_) return;
  if (!started_ || failed_ || !client_) {
    closed_ = true;
    invoke(closed_cb_);
    return;
  }
  client_->stop_reading();
  client_->close_after_flush();
}

void SessionForwarder::abort() {
  if (closed_) return;
  if (!client_) {
    closed_ = true;
    invoke(closed_cb_);
    return;
  }
  client_->abort();
}

void SessionForwarder::pause_responses() {
  if (client_) client_->pause_reads();
}

void SessionForwarder::resume_responses() {
  if (client_) client_->resume_reads();
}

bool SessionForwarder::responses_paused() const {
  return client_ && client_->reads_paused();
}

size_t SessionForwarder::queued_bytes() const {
  return client_ ? client_->queued_bytes() : 0;
}

uint64_t SessionForwarder::bytes_forwarded() const {
  return client_ ? client_->bytes_written() : 0;
}

uint64_t SessionForwarder::bytes_received() const {
  return client_ ? client_->bytes_read() : 0;
}

uint64_t SessionForwarder::connects() const { return client_ ? client_->connects() : 0; }

void SessionForwarder::handle_data(std::vector<uint8_t> bytes) {
  if (closed_ || failed_ || bytes.empty()) return;
  if (response_cb_) response_cb_(std::move(bytes));
}

void SessionForwarder::handle_drained() {
  if (closed_) return;
  invoke(drained_cb_);
}

void SessionForwarder::handle_closed() {
  if (closed_) return;
  closed_ = true;
  invoke(closed_cb_);
}

void SessionForwarder::handle_error(const std::string& message) {
  if (closed_) return;

  if (config_.reconnect && !config_.link_connections && client_ && client_->connects() > 0 &&
      reconnects_ < config_.max_reconnects) {
    ++reconnects_;
    log_printf(LogLevel::Warn, "lost connection to %s:%u (%s), reconnecting (%u/%u)",
               config_.host.c_str(), static_cast<unsigned>(config_.port), message.c_str(), reconnects_,
               config_.max_reconnects);
    client_->restart();
    return;
  }

  failed_ = true;
  if (config_.link_connections) {
    log_printf(LogLevel::Warn,
               "forwarding to %s:%u stopped: %s (closing client connection)",
               config_.host.c_str(), static_cast<unsigned>(config_.port), message.c_str());
  } else {
    log_printf(LogLevel::Warn, "forwarding to %s:%u stopped: %s (recording continues)",
               config_.host.c_str(), static_cast<unsigned>(config_.port), message.c_str());
  }
  if (client_) client_->abort();
  invoke_error(message);
}

void SessionForwarder::invoke(Callback& callback) {
  if (!callback) return;
  auto fn = callback;
  asio::post(executor_, [fn = std::move(fn)]() mutable { fn(); });
}

void SessionForwarder::invoke_error(const std::string& message) {
  if (!error_cb_) return;
  auto fn = error_cb_;
  asio::post(executor_, [fn = std::move(fn), message]() mutable { fn(message); });
}

}  // namespace prp
