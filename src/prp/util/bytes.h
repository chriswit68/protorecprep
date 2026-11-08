#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace prp {

// Explicit little-endian encoding helpers. The record format is LE on every
// platform so that captures are portable between Linux and macOS.
inline void put_u16le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void put_u64le(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t get_u16le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t get_u32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t get_u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

inline void append_u16le(std::vector<uint8_t>& out, uint16_t v) {
  uint8_t b[2];
  put_u16le(b, v);
  out.insert(out.end(), b, b + 2);
}
inline void append_u32le(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t b[4];
  put_u32le(b, v);
  out.insert(out.end(), b, b + 4);
}
inline void append_u64le(std::vector<uint8_t>& out, uint64_t v) {
  uint8_t b[8];
  put_u64le(b, v);
  out.insert(out.end(), b, b + 8);
}
inline void append_bytes(std::vector<uint8_t>& out, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  out.insert(out.end(), p, p + n);
}

inline uint64_t load_u64le_unaligned(const void* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap64(v);
#endif
  return v;
}

}  // namespace prp
