#include "prp/net/framer.h"

#include <algorithm>
#include <cstring>

#include "prp/lib.h"

namespace prp {

FrameValidator::FrameValidator(rec::Framing framing, size_t max_frame)
    : framing_(framing), max_frame_(max_frame == 0 ? 1 : max_frame) {}

void FrameValidator::reset() {
  state_ = State::Length;
  value_ = 0;
  shift_ = 0;
  varint_bytes_ = 0;
  remaining_ = 0;
  header_bytes_ = 0;
}

bool FrameValidator::feed(const uint8_t* data, size_t n, std::string& err) {
  if (framing_ == rec::Framing::Raw) return true;
  size_t i = 0;
  while (i < n) {
    if (state_ == State::Payload) {
      const uint64_t take = std::min<uint64_t>(remaining_, n - i);
      remaining_ -= take;
      i += static_cast<size_t>(take);
      if (remaining_ == 0) state_ = State::Length;
      continue;
    }
    if (framing_ == rec::Framing::TypedLengthLE) {
      if (state_ != State::TypedHeader) {
        state_ = State::TypedHeader;
        header_bytes_ = 0;
      }
      const size_t take = std::min<size_t>(kTypedHeaderSize - header_bytes_, n - i);
      std::memcpy(header_ + header_bytes_, data + i, take);
      header_bytes_ += take;
      i += take;
      if (header_bytes_ < kTypedHeaderSize) continue;
      const uint32_t length = get_u32le(header_ + 4);
      if (length > max_frame_) {
        err = "frame length " + std::to_string(length) + " exceeds --max-frame (" +
              std::to_string(max_frame_) + ")";
        return false;
      }
      header_bytes_ = 0;
      remaining_ = length;
      ++frames_;
      state_ = remaining_ == 0 ? State::Length : State::Payload;
      continue;
    }
    const uint8_t byte = data[i++];
    if (varint_bytes_ >= 10 || shift_ >= 64) {
      err = "varint length prefix exceeds 10 bytes";
      return false;
    }
    value_ |= static_cast<uint64_t>(byte & 0x7Fu) << shift_;
    ++varint_bytes_;
    if ((byte & 0x80u) != 0) {
      shift_ += 7;
      continue;
    }
    if (value_ > max_frame_) {
      err = "frame length " + std::to_string(value_) + " exceeds --max-frame (" +
            std::to_string(max_frame_) + ")";
      return false;
    }
    remaining_ = value_;
    value_ = 0;
    shift_ = 0;
    varint_bytes_ = 0;
    ++frames_;
    state_ = remaining_ == 0 ? State::Length : State::Payload;
  }
  return true;
}

FrameReader::FrameReader(rec::Framing framing, size_t max_frame)
    : framing_(framing), max_frame_(max_frame == 0 ? 1 : max_frame) {}

void FrameReader::append(const uint8_t* data, size_t n) {
  buf_.insert(buf_.end(), data, data + n);
}

void FrameReader::clear() {
  buf_.clear();
  off_ = 0;
}

void FrameReader::compact() {
  if (off_ == 0) return;
  if (off_ == buf_.size()) {
    buf_.clear();
    off_ = 0;
    return;
  }
  if (off_ >= 64 * 1024 && off_ * 2 >= buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off_));
    off_ = 0;
  }
}

FrameReader::Result FrameReader::next(std::string_view& frame, std::string& err) {
  compact();
  size_t pos = off_;
  uint64_t value = 0;
  int shift = 0;
  int bytes = 0;
  while (true) {
    if (pos >= buf_.size()) {
      compact();
      return Result::NeedMore;
    }
    const uint8_t byte = buf_[pos++];
    if (bytes >= 10 || shift >= 64) {
      err = "varint length prefix exceeds 10 bytes";
      off_ = pos;
      return Result::Error;
    }
    value |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
    ++bytes;
    if ((byte & 0x80u) != 0) {
      shift += 7;
      continue;
    }
    break;
  }
  if (value > max_frame_) {
    err = "frame length " + std::to_string(value) + " exceeds limit";
    off_ = pos;
    return Result::Error;
  }
  if (buf_.size() - pos < value) {
    // Leave off_ at the frame start so a future append can complete it.
    compact();
    return Result::NeedMore;
  }
  frame = std::string_view(reinterpret_cast<const char*>(buf_.data() + pos), static_cast<size_t>(value));
  off_ = pos + static_cast<size_t>(value);
  return Result::Frame;
}

FrameReader::Result FrameReader::next(Frame& frame, std::string& err) {
  if (framing_ != rec::Framing::TypedLengthLE) {
    std::string_view payload;
    const Result result = next(payload, err);
    if (result == Result::Frame) {
      frame.wire = payload;
      frame.payload = payload;
      frame.type_id = 0;
      frame.has_type = false;
    }
    return result;
  }

  compact();
  const size_t pos = off_;
  if (buf_.size() - pos < kTypedHeaderSize) {
    compact();
    return Result::NeedMore;
  }
  const uint32_t type_id = get_u32le(buf_.data() + pos);
  const uint32_t length = get_u32le(buf_.data() + pos + 4);
  if (length > max_frame_) {
    err = "frame length " + std::to_string(length) + " exceeds limit";
    off_ = pos + kTypedHeaderSize;
    return Result::Error;
  }
  if (buf_.size() - pos - kTypedHeaderSize < length) {
    compact();
    return Result::NeedMore;
  }
  frame.wire = std::string_view(reinterpret_cast<const char*>(buf_.data() + pos),
                                kTypedHeaderSize + length);
  frame.payload = std::string_view(
      reinterpret_cast<const char*>(buf_.data() + pos + kTypedHeaderSize), length);
  frame.type_id = type_id;
  frame.has_type = true;
  off_ = pos + kTypedHeaderSize + length;
  return Result::Frame;
}

void append_varint(std::string& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

std::string frame_message(std::string_view payload) {
  std::string out;
  out.reserve(payload.size() + 10);
  append_varint(out, payload.size());
  out.append(payload.data(), payload.size());
  return out;
}

std::string frame_typed_message(uint32_t type_id, std::string_view payload) {
  std::string out;
  out.resize(kTypedHeaderSize + payload.size());
  put_u32le(reinterpret_cast<uint8_t*>(out.data()), type_id);
  put_u32le(reinterpret_cast<uint8_t*>(out.data()) + 4, static_cast<uint32_t>(payload.size()));
  if (!payload.empty()) std::memcpy(out.data() + kTypedHeaderSize, payload.data(), payload.size());
  return out;
}

}  // namespace prp
