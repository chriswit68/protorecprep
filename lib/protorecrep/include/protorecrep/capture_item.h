#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace protorecrep {

enum class ItemKind : uint8_t { StreamOpen = 1, Data = 2, StreamClose = 3, Marker = 4 };

enum class Direction : uint8_t { In = 0, Out = 1 };

enum class CloseReason : uint8_t {
  PeerClosed = 0,
  LocalShutdown = 1,
  ProtocolError = 2,
  IdleTimeout = 3,
  ServerShutdown = 4,
  Overflow = 5,
  AcceptRejected = 6,
  SendError = 7,
  ForwardError = 8,
};

const char* close_reason_name(CloseReason reason);

// Immutable unit of work shared between the recorder writer thread and the
// optional JSON parser thread. Ownership is via shared_ptr; the item is
// released once both consumers are done with it.
struct CaptureItem {
  ItemKind kind = ItemKind::Data;
  uint32_t stream_id = 0;
  Direction dir = Direction::In;
  int64_t ts_ns = 0;
  uint8_t close_reason = 0;
  std::string peer;
  std::string type_name;
  std::vector<uint8_t> data;

  size_t accounted_bytes() const {
    return sizeof(CaptureItem) + peer.capacity() + type_name.capacity() + data.capacity();
  }
};

using ItemPtr = std::shared_ptr<const CaptureItem>;

ItemPtr make_stream_open(uint32_t stream_id, int64_t ts_ns, std::string peer, std::string type_name);
ItemPtr make_data(uint32_t stream_id, Direction dir, int64_t ts_ns, std::vector<uint8_t> bytes,
                  std::string type_name = {});
ItemPtr make_stream_close(uint32_t stream_id, CloseReason reason, int64_t ts_ns);
ItemPtr make_marker(int64_t ts_ns, std::string text);

}  // namespace protorecrep
