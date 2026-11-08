#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prp/record/format.h"

namespace prp {

// Incremental validation of the [varint length][payload] framing without
// buffering payload bytes. Used on the capture hot path so oversized or
// malformed frames are rejected early without allocating.
class FrameValidator {
 public:
  FrameValidator(rec::Framing framing, size_t max_frame);

  bool feed(const uint8_t* data, size_t n, std::string& err);
  void reset();
  uint64_t frames() const { return frames_; }
  bool in_partial_frame() const { return state_ == State::Payload || varint_bytes_ > 0; }

 private:
  enum class State { Length, Payload };

  rec::Framing framing_;
  size_t max_frame_;
  State state_ = State::Length;
  uint64_t value_ = 0;
  int shift_ = 0;
  int varint_bytes_ = 0;
  uint64_t remaining_ = 0;
  uint64_t frames_ = 0;
};

// Full reassembly buffer for workers that need message boundaries (JSON
// conversion). The view returned by next() is valid until the next call.
class FrameReader {
 public:
  explicit FrameReader(size_t max_frame);

  void append(const uint8_t* data, size_t n);
  enum class Result { NeedMore, Frame, Error };

  Result next(std::string_view& frame, std::string& err);
  void clear();
  size_t buffered_bytes() const { return buf_.size() - off_; }
  std::string_view buffered_view() const {
    return std::string_view(reinterpret_cast<const char*>(buf_.data() + off_), buf_.size() - off_);
  }

 private:
  void compact();

  std::vector<uint8_t> buf_;
  size_t off_ = 0;
  size_t max_frame_;
};

// Helpers used by the sender and tests.
void append_varint(std::string& out, uint64_t value);
std::string frame_message(std::string_view payload);

}  // namespace prp
