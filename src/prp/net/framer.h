#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prp/record/format.h"

namespace prp {

// Wire header of the "typed-length-le" framing: [u32 LE type][u32 LE length].
inline constexpr size_t kTypedHeaderSize = 8;

// Incremental validation of the wire framing without buffering payload bytes.
// Used on the capture hot path so oversized or malformed frames are rejected
// early without allocating.
class FrameValidator {
 public:
  FrameValidator(rec::Framing framing, size_t max_frame);

  bool feed(const uint8_t* data, size_t n, std::string& err);
  void reset();
  uint64_t frames() const { return frames_; }
  bool in_partial_frame() const {
    return state_ == State::Payload || varint_bytes_ > 0 || header_bytes_ > 0;
  }

 private:
  enum class State { Length, TypedHeader, Payload };

  rec::Framing framing_;
  size_t max_frame_;
  State state_ = State::Length;
  uint64_t value_ = 0;
  int shift_ = 0;
  int varint_bytes_ = 0;
  uint64_t remaining_ = 0;
  uint64_t frames_ = 0;
  uint8_t header_[kTypedHeaderSize] = {};
  size_t header_bytes_ = 0;
};

// Full reassembly buffer for workers that need message boundaries. In the
// typed framing each frame also carries the message type id from its header.
class FrameReader {
 public:
  explicit FrameReader(rec::Framing framing, size_t max_frame);

  void append(const uint8_t* data, size_t n);
  enum class Result { NeedMore, Frame, Error };

  // Plain payload view. Valid for varint framing (and raw, where every byte is
  // treated as a single frame); returns full frames including the 8-byte
  // header for typed framing, so prefer the Frame overload there.
  Result next(std::string_view& frame, std::string& err);

  struct Frame {
    std::string_view wire;     // complete frame as it appears on the wire
    std::string_view payload;  // protobuf payload without the typed header
    uint32_t type_id = 0;
    bool has_type = false;
  };
  Result next(Frame& frame, std::string& err);

  void clear();
  size_t buffered_bytes() const { return buf_.size() - off_; }
  std::string_view buffered_view() const {
    return std::string_view(reinterpret_cast<const char*>(buf_.data() + off_), buf_.size() - off_);
  }

 private:
  void compact();

  rec::Framing framing_;
  std::vector<uint8_t> buf_;
  size_t off_ = 0;
  size_t max_frame_;
};

// Helpers used by the sender and tests.
void append_varint(std::string& out, uint64_t value);
std::string frame_message(std::string_view payload);
// Builds [u32 LE type_id][u32 LE payload length][payload].
std::string frame_typed_message(uint32_t type_id, std::string_view payload);

}  // namespace prp
