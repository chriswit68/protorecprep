#include "prp/net/server.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>

#include <sys/socket.h>

#include "prp/net/framer.h"
#include "prp/util/log.h"
#include "prp/util/text.h"

namespace prp {
namespace asio = boost::asio;
using asio::ip::tcp;
using ErrorCode = boost::system::error_code;

class Session : public std::enable_shared_from_this<Session> {
 public:
  Session(asio::io_context& io, tcp::socket socket, uint32_t id, Server& server)
      : server_(server),
        id_(id),
        strand_(asio::make_strand(io)),
        socket_(std::move(socket)),
        read_buffer_(server.config().recv_buffer == 0 ? 65536 : server.config().recv_buffer),
        validator_(server.config().framing, server.config().max_frame),
        idle_timer_(strand_),
        once_timer_(strand_),
        every_timer_(strand_),
        forward_watchdog_(strand_) {
    const ServerConfig& config = server_.config();
    // In typed framing the message type comes from each frame header, so the
    // stream itself has no single type.
    if (config.framing != rec::Framing::TypedLengthLE) {
      open_type_name_ = !config.default_type_name.empty() ? config.default_type_name
                                                          : config.send.type_name;
    }
    if (config.forward.enabled) {
      forwarder_ = std::make_shared<SessionForwarder>(strand_, config.forward);
    }
  }

  void start() {
    auto self = shared_from_this();
    if (forwarder_) {
      std::weak_ptr<Session> weak = self;
      forwarder_->set_drained_callback([weak] {
        if (auto session = weak.lock()) session->on_forward_drained();
      });
      forwarder_->set_closed_callback([weak] {
        if (auto session = weak.lock()) session->on_forward_closed();
      });
      forwarder_->set_error_callback([weak](const std::string&) {
        if (auto session = weak.lock()) session->on_forward_error();
      });
      forwarder_->set_response_callback([weak](std::vector<uint8_t> bytes) {
        if (auto session = weak.lock()) session->on_response(std::move(bytes));
      });
    }
    asio::post(strand_, [self] {
      if (self->closing_) return;
      self->opened_ns_ = now_wall_ns();
      self->started_ = true;
      self->submit(make_stream_open(self->id_, self->opened_ns_, self->peer_string(),
                                    self->open_type_name_));
      if (self->closing_) return;
      self->arm_idle_timer();
      self->schedule_sends();
      self->do_read();
    });
  }

  void close(CloseReason reason) {
    auto self = shared_from_this();
    asio::post(strand_, [self, reason] { self->do_close(reason); });
  }

  uint32_t id() const { return id_; }

 private:
  using Strand = asio::strand<asio::io_context::executor_type>;

  std::string peer_string() const {
    ErrorCode ec;
    const auto ep = socket_.remote_endpoint(ec);
    if (ec) return "unknown";
    return ep.address().to_string() + ":" + std::to_string(ep.port());
  }

  void submit(const ItemPtr& item) {
    const PushResult result = server_.recorder().submit(item);
    if (result == PushResult::Closed) {
      do_close(CloseReason::ServerShutdown);
    } else if (result == PushResult::Overflow) {
      log_printf(LogLevel::Warn, "stream %u: capture queue overflow, closing connection", id_);
      do_close(CloseReason::Overflow);
    }
  }

  void do_read() {
    if (closing_) return;
    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer(read_buffer_), asio::bind_executor(strand_, [self](const ErrorCode& ec, size_t n) {
          self->on_read(ec, n);
        }));
  }

  void on_read(const ErrorCode& ec, size_t n) {
    if (closing_) return;
    if (ec) {
      if (ec == asio::error::eof || ec == asio::error::connection_reset) {
        do_close(CloseReason::PeerClosed);
      } else if (ec == asio::error::operation_aborted) {
        do_close(CloseReason::ServerShutdown);
      } else {
        log_printf(LogLevel::Debug, "stream %u read error: %s", id_, ec.message().c_str());
        do_close(CloseReason::PeerClosed);
      }
      return;
    }
    if (n == 0) {
      do_read();
      return;
    }

    arm_idle_timer();

    // Capture first: even bytes that violate framing must be preserved so the
    // recording stays a faithful byte stream.
    {
      std::vector<uint8_t> bytes(read_buffer_.begin(),
                                 read_buffer_.begin() + static_cast<std::ptrdiff_t>(n));
      submit(make_data(id_, Direction::In, now_wall_ns(), std::move(bytes),
                       server_.config().default_type_name));
    }
    if (closing_) return;

    // Forward the raw inbound bytes before framing validation. Forwarding never
    // includes locally generated outbound messages.
    if (forwarder_ && !forwarder_->failed() && !forwarder_->closed()) {
      std::vector<uint8_t> bytes(read_buffer_.begin(),
                                 read_buffer_.begin() + static_cast<std::ptrdiff_t>(n));
      forwarder_->forward(std::move(bytes));
    }

    std::string err;
    if (!validator_.feed(read_buffer_.data(), n, err)) {
      log_printf(LogLevel::Warn, "stream %u protocol error: %s", id_, err.c_str());
      do_close(CloseReason::ProtocolError);
      return;
    }

    // Pause source reads while the forwarding backlog is full. The drained
    // callback resumes reading, so no bytes are dropped.
    if (forwarder_ && forwarder_->active() &&
        forwarder_->queued_bytes() >= server_.config().forward.queue_limit) {
      paused_ = true;
      idle_timer_.cancel();
      return;
    }
    do_read();
  }

  void arm_idle_timer() {
    const auto timeout = server_.config().idle_timeout;
    if (timeout.count() <= 0) return;
    auto self = shared_from_this();
    idle_timer_.expires_after(timeout);
    idle_timer_.async_wait(asio::bind_executor(strand_, [self](const ErrorCode& ec) {
      if (!ec) self->do_close(CloseReason::IdleTimeout);
    }));
  }

  void schedule_sends() {
    const SendSpec& spec = server_.config().send;
    if (!spec.enabled) return;
    if (spec.count > 0 && sent_count_ >= spec.count) return;
    if (spec.has_once_after) {
      auto self = shared_from_this();
      once_timer_.expires_after(spec.once_after);
      once_timer_.async_wait(asio::bind_executor(strand_, [self](const ErrorCode& ec) {
        if (ec) return;
        self->send_message();
        if (self->server_.config().send.has_every) self->schedule_every();
      }));
    } else if (spec.has_every) {
      schedule_every();
    }
  }

  void schedule_every() {
    const SendSpec& spec = server_.config().send;
    if (closing_ || spec.every.count() <= 0) return;
    if (spec.count > 0 && sent_count_ >= spec.count) return;
    auto self = shared_from_this();
    every_timer_.expires_after(spec.every);
    every_timer_.async_wait(asio::bind_executor(strand_, [self](const ErrorCode& ec) {
      if (ec) return;
      if (self->closing_) return;
      self->send_message();
      self->schedule_every();
    }));
  }

  void send_message() {
    if (closing_) return;
    const SendSpec& spec = server_.config().send;
    if (!spec.enabled || spec.framed_bytes.empty()) return;
    if (spec.count > 0 && sent_count_ >= spec.count) return;

    // Bound the per-connection write backlog; drop sends rather than growing
    // memory without limit.
    constexpr size_t kMaxWriteBacklog = 4 * 1024 * 1024;
    if (write_queue_bytes_ + spec.framed_bytes.size() > kMaxWriteBacklog) {
      if (dropped_sends_++ % 100 == 0) {
        log_printf(LogLevel::Warn, "stream %u: outbound backlog full, dropped %llu messages", id_,
                   static_cast<unsigned long long>(dropped_sends_));
      }
      return;
    }

    ++sent_count_;
    {
      std::vector<uint8_t> copy(spec.framed_bytes.begin(), spec.framed_bytes.end());
      submit(make_data(id_, Direction::Out, now_wall_ns(), std::move(copy), spec.type_name));
      if (closing_) return;
    }

    write_queue_bytes_ += spec.framed_bytes.size();
    write_queue_.push_back(spec.framed_bytes);
    do_write();
  }

  void do_write() {
    if (closing_ || write_in_progress_ || write_queue_.empty()) return;
    write_in_progress_ = true;
    auto self = shared_from_this();
    socket_.async_write_some(asio::buffer(write_queue_.front()),
                             asio::bind_executor(strand_, [self](const ErrorCode& ec, size_t n) {
                               self->on_write(ec, n);
                             }));
  }

  void on_write(const ErrorCode& ec, size_t n) {
    write_in_progress_ = false;
    if (closing_) return;
    if (ec) {
      log_printf(LogLevel::Debug, "stream %u write error: %s", id_, ec.message().c_str());
      do_close(CloseReason::SendError);
      return;
    }
    if (n < write_queue_.front().size()) {
      // async_write_some may complete partially; keep the remainder.
      write_queue_.front().erase(0, n);
      write_queue_bytes_ -= n;
    } else {
      write_queue_bytes_ -= write_queue_.front().size();
      write_queue_.pop_front();
    }
    do_write();

    // The client has made room, so release the backpressure on the target.
    if (forwarder_ && forwarder_->active() && forwarder_->responses_paused() &&
        write_queue_bytes_ < server_.config().forward.queue_limit) {
      forwarder_->resume_responses();
    }
  }

  void on_response(std::vector<uint8_t> bytes) {
    if (closing_ || bytes.empty()) return;

    // Queue the bytes for the client first, then record a copy. The write
    // queue holds payloads as strings; the capture item takes the buffer.
    std::string queued(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    write_queue_bytes_ += bytes.size();
    write_queue_.push_back(std::move(queued));
    submit(make_data(id_, Direction::Out, now_wall_ns(), std::move(bytes),
                     server_.config().default_type_name));
    if (closing_) return;
    do_write();

    // Backpressure on the target while the client backlog is full: pause
    // reads until the write queue drains again. No bytes are lost.
    if (forwarder_ && forwarder_->active() && write_queue_bytes_ >= server_.config().forward.queue_limit) {
      forwarder_->pause_responses();
    }
  }

  void do_close(CloseReason reason) {
    if (closing_) return;
    closing_ = true;

    ErrorCode ec;
    idle_timer_.cancel();
    once_timer_.cancel();
    every_timer_.cancel();
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);

    if (started_) {
      submit(make_stream_close(id_, reason, now_wall_ns()));
    }
    log_printf(LogLevel::Debug, "stream %u closed (%s)", id_, close_reason_name(reason));

    // Keep the session (and its captured stream metadata) alive until the
    // forwarding backlog has drained. Recording has already been committed.
    if (forwarder_ && forwarder_->active()) {
      forwarder_->close();
      arm_forward_watchdog();
      return;
    }
    finalize();
  }

  void on_forward_drained() {
    if (closing_ || !paused_) return;
    paused_ = false;
    arm_idle_timer();
    do_read();
  }

  void on_forward_error() {
    // Forwarding stopped, but recording continues. Release any backpressure
    // that was imposed by the forwarding queue.
    if (closing_ || !paused_) return;
    paused_ = false;
    arm_idle_timer();
    do_read();
  }

  void on_forward_closed() {
    if (closing_) {
      finalize();
      return;
    }
    if (!paused_) return;
    paused_ = false;
    arm_idle_timer();
    do_read();
  }

  void arm_forward_watchdog() {
    constexpr auto kFinishTimeout = std::chrono::seconds(30);
    forward_watchdog_.expires_after(kFinishTimeout);
    auto self = shared_from_this();
    forward_watchdog_.async_wait(asio::bind_executor(strand_, [self](const ErrorCode& ec) {
      if (ec || self->finalized_) return;
      log_printf(LogLevel::Warn, "stream %u: timed out flushing forward queue", self->id_);
      if (self->forwarder_) self->forwarder_->abort();
      self->finalize();
    }));
  }

  void finalize() {
    if (finalized_) return;
    finalized_ = true;
    forward_watchdog_.cancel();
    if (forwarder_) {
      server_.note_forward_result(forwarder_->started(), forwarder_->bytes_forwarded(),
                                  forwarder_->connects(), forwarder_->failed(),
                                  forwarder_->bytes_received());
    }
    server_.on_session_closed(id_);
  }

  Server& server_;
  uint32_t id_;
  Strand strand_;
  tcp::socket socket_;
  std::vector<uint8_t> read_buffer_;
  FrameValidator validator_;
  asio::steady_timer idle_timer_;
  asio::steady_timer once_timer_;
  asio::steady_timer every_timer_;
  asio::steady_timer forward_watchdog_;
  std::deque<std::string> write_queue_;
  size_t write_queue_bytes_ = 0;
  bool write_in_progress_ = false;
  bool closing_ = false;
  bool started_ = false;
  std::string open_type_name_;
  int64_t opened_ns_ = 0;
  uint64_t sent_count_ = 0;
  uint64_t dropped_sends_ = 0;
  std::shared_ptr<SessionForwarder> forwarder_;
  bool paused_ = false;
  bool finalized_ = false;
};

Server::Server(asio::io_context& io, ServerConfig config, Recorder& recorder)
    : io_(io), config_(std::move(config)), recorder_(recorder), acceptor_(io) {}

Server::~Server() = default;

bool Server::open(std::string& err) {
  const std::string host = config_.listen_host.empty() ? "0.0.0.0" : config_.listen_host;
  tcp::resolver resolver(io_);
  ErrorCode ec;
  const auto results = resolver.resolve(host, std::to_string(config_.listen_port), tcp::resolver::passive, ec);
  if (ec || results.empty()) {
    err = "cannot resolve listen address " + host + ":" + std::to_string(config_.listen_port) + ": " +
          ec.message();
    return false;
  }
  const tcp::endpoint endpoint = results.begin()->endpoint();

  acceptor_.open(endpoint.protocol(), ec);
  if (ec) {
    err = "cannot open acceptor: " + ec.message();
    return false;
  }
  acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) {
    err = "cannot set SO_REUSEADDR: " + ec.message();
    return false;
  }
  acceptor_.bind(endpoint, ec);
  if (ec) {
    err = "cannot bind " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()) + ": " +
          ec.message();
    return false;
  }
  acceptor_.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    err = "cannot listen: " + ec.message();
    return false;
  }
  log_printf(LogLevel::Info, "listening on %s:%u (framing=%s)",
             endpoint.address().to_string().c_str(), static_cast<unsigned>(endpoint.port()),
             rec::framing_name(config_.framing));
  return true;
}

void Server::start() { do_accept(); }

void Server::do_accept() {
  if (stopping_.load(std::memory_order_acquire)) return;
  acceptor_.async_accept([this](const ErrorCode& ec, tcp::socket socket) {
    if (ec) {
      if (ec == asio::error::operation_aborted || stopping_.load(std::memory_order_acquire)) return;
      log_printf(LogLevel::Error, "accept failed: %s", ec.message().c_str());
      if (!stopping_.load(std::memory_order_acquire)) do_accept();
      return;
    }
    if (stopping_.load(std::memory_order_acquire)) {
      ErrorCode ignored;
      socket.close(ignored);
      return;
    }

    ErrorCode opt_ec;
    socket.set_option(tcp::no_delay(true), opt_ec);
    socket.set_option(asio::socket_base::keep_alive(true), opt_ec);
    if (config_.recv_buffer > 0) {
      const int sz = static_cast<int>(config_.recv_buffer);
      ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    }

    std::shared_ptr<Session> session;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!stopping_.load(std::memory_order_acquire) && sessions_.size() < config_.max_connections) {
        const uint32_t id = next_session_id_++;
        session = std::make_shared<Session>(io_, std::move(socket), id, *this);
        sessions_.emplace(id, session);
        total_accepted_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (!session) {
      ErrorCode ignored;
      socket.close(ignored);
      log_printf(LogLevel::Warn, "connection rejected (limit %zu reached)", config_.max_connections);
      do_accept();
      return;
    }

    log_printf(LogLevel::Debug, "stream %u accepted", session->id());
    session->start();
    if (stopping_.load(std::memory_order_acquire)) {
      session->close(CloseReason::ServerShutdown);
    }
    do_accept();
  });
}

void Server::shutdown() {
  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
    for (auto& [id, session] : sessions_) sessions.push_back(session);
  }
  asio::post(io_, [this] {
    ErrorCode ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
  });
  log_printf(LogLevel::Info, "shutdown requested (%zu active streams)", sessions.size());
  for (auto& session : sessions) session->close(CloseReason::ServerShutdown);
  if (sessions.empty()) fire_stopped();
}

void Server::on_session_closed(uint32_t id) {
  bool should_fire = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(id);
    should_fire = stopping_.load(std::memory_order_acquire) && sessions_.empty();
  }
  if (should_fire) fire_stopped();
}

void Server::fire_stopped() {
  if (stopped_fired_.exchange(true)) return;
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callback = stopped_callback_;
  }
  if (callback) callback();
}

void Server::set_stopped_callback(std::function<void()> callback) {
  bool fire_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_callback_ = std::move(callback);
    fire_now = stopping_.load(std::memory_order_acquire) && sessions_.empty();
  }
  if (fire_now) fire_stopped();
}

size_t Server::active_sessions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

void Server::note_forward_result(bool started, uint64_t bytes, uint64_t connects, bool failed,
                                 uint64_t response_bytes) {
  if (!started) return;
  forward_streams_.fetch_add(1, std::memory_order_relaxed);
  forward_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  forward_response_bytes_.fetch_add(response_bytes, std::memory_order_relaxed);
  forward_connects_.fetch_add(connects, std::memory_order_relaxed);
  if (failed) forward_failures_.fetch_add(1, std::memory_order_relaxed);
}

ForwardStats Server::forward_stats() const {
  ForwardStats stats;
  stats.streams = forward_streams_.load(std::memory_order_relaxed);
  stats.bytes = forward_bytes_.load(std::memory_order_relaxed);
  stats.response_bytes = forward_response_bytes_.load(std::memory_order_relaxed);
  stats.connects = forward_connects_.load(std::memory_order_relaxed);
  stats.failures = forward_failures_.load(std::memory_order_relaxed);
  return stats;
}

}  // namespace prp
