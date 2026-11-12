#include "prp/core/item_queue.h"

#include <utility>

namespace prp {
namespace {

std::shared_ptr<CaptureItem> make_item(ItemKind kind, uint32_t stream_id, Direction dir, int64_t ts_ns) {
  auto item = std::make_shared<CaptureItem>();
  item->kind = kind;
  item->stream_id = stream_id;
  item->dir = dir;
  item->ts_ns = ts_ns;
  return item;
}

}  // namespace

const char* close_reason_name(CloseReason reason) {
  switch (reason) {
    case CloseReason::PeerClosed: return "peer_closed";
    case CloseReason::LocalShutdown: return "local_shutdown";
    case CloseReason::ProtocolError: return "protocol_error";
    case CloseReason::IdleTimeout: return "idle_timeout";
    case CloseReason::ServerShutdown: return "server_shutdown";
    case CloseReason::Overflow: return "overflow";
    case CloseReason::AcceptRejected: return "accept_rejected";
    case CloseReason::SendError: return "send_error";
    case CloseReason::ForwardError: return "forward_error";
  }
  return "unknown";
}

ItemPtr make_stream_open(uint32_t stream_id, int64_t ts_ns, std::string peer, std::string type_name) {
  auto item = make_item(ItemKind::StreamOpen, stream_id, Direction::In, ts_ns);
  item->peer = std::move(peer);
  item->type_name = std::move(type_name);
  return item;
}

ItemPtr make_data(uint32_t stream_id, Direction dir, int64_t ts_ns, std::vector<uint8_t> bytes,
                  std::string type_name) {
  auto item = make_item(ItemKind::Data, stream_id, dir, ts_ns);
  item->data = std::move(bytes);
  item->type_name = std::move(type_name);
  return item;
}

ItemPtr make_stream_close(uint32_t stream_id, CloseReason reason, int64_t ts_ns) {
  auto item = make_item(ItemKind::StreamClose, stream_id, Direction::In, ts_ns);
  item->close_reason = static_cast<uint8_t>(reason);
  return item;
}

ItemPtr make_marker(int64_t ts_ns, std::string text) {
  auto item = make_item(ItemKind::Marker, 0, Direction::In, ts_ns);
  item->peer = std::move(text);
  return item;
}

}  // namespace prp
