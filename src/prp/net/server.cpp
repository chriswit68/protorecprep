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

namespace {

bool in_type_filter(const std::vector<std::string>& filter, const std::string& type_name) {
  if (type_name.empty()) return false;
  return std::find(filter.begin(), filter.end(), type_name) != filter.end();
}

}  // namespace

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
        every_timer_(strand_) {
    const ServerConfig& config = server_.config();
    const std::string& outbound_type =
        config.send.enabled ? config.send.type_name : config.default_type_name;
    if (!config.type_filter.empty()) {
      record_inbound_ = in_type_filter(config.type_filter, config.default_type_name);
      record_outbound_ = in_type_filter(config.type_filter, outbound_type);
      open_type_name_ = record_inbound_ ? config.default_type_name : outbound_type;
    } else {
      open_type_name_ = config.default_type_name.empty() && config.send.enabled
                            ? config.send.type_name
                            : config.default_type_name;
    }
  }

  void start() {
    auto self = shared_from_this();
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

  bool should_record(const CaptureItem& item) const {
    if (item.kind == ItemKind::StreamOpen || item.kind == ItemKind::StreamClose) {
      // Keep stream metadata as long as at least one direction is captured.
      return record_inbound_ || record_outbound_;
    }
    return item.dir == Direction::Out ? record_outbound_ : record_inbound_;
  }

  void submit(const ItemPtr& item) {
    if (!should_record(*item)) return;
    const PushResult result = server_.recorder().submit(item);
    if (server_.json()) server_.json()->submit(item);
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
    if (record_inbound_) {
      std::vector<uint8_t> bytes(read_buffer_.begin(),
                                 read_buffer_.begin() + static_cast<std::ptrdiff_t>(n));
      submit(make_data(id_, Direction::In, now_wall_ns(), std::move(bytes),
                       server_.config().default_type_name));
    }
    if (closing_) return;

    std::string err;
    if (!validator_.feed(read_buffer_.data(), n, err)) {
      log_printf(LogLevel::Warn, "stream %u protocol error: %s", id_, err.c_str());
      do_close(CloseReason::ProtocolError);
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
    if (record_outbound_) {
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
      do_write();
      return;
    }
    write_queue_bytes_ -= write_queue_.front().size();
    write_queue_.pop_front();
    do_write();
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
  std::deque<std::string> write_queue_;
  size_t write_queue_bytes_ = 0;
  bool write_in_progress_ = false;
  bool closing_ = false;
  bool started_ = false;
  bool record_inbound_ = true;
  bool record_outbound_ = true;
  std::string open_type_name_;
  int64_t opened_ns_ = 0;
  uint64_t sent_count_ = 0;
  uint64_t dropped_sends_ = 0;
};

Server::Server(asio::io_context& io, ServerConfig config, Recorder& recorder, JsonWriter* json)
    : io_(io), config_(std::move(config)), recorder_(recorder), json_(json), acceptor_(io) {}

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
  log_printf(LogLevel::Info, "listening on %s:%u (framing=%s, json=%s)",
             endpoint.address().to_string().c_str(), static_cast<unsigned>(endpoint.port()),
             rec::framing_name(config_.framing), json_ ? "on" : "off");
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

}  // namespace prp
