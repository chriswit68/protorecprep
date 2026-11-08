#include "prp/util/base64.h"

#include <array>

namespace prp {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string base64_encode(const uint8_t* data, size_t n) {
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(kAlphabet[(v >> 6) & 0x3F]);
    out.push_back(kAlphabet[v & 0x3F]);
  }
  if (i < n) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < n) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    out.push_back(kAlphabet[(v >> 18) & 0x3F]);
    out.push_back(kAlphabet[(v >> 12) & 0x3F]);
    out.push_back(i + 1 < n ? kAlphabet[(v >> 6) & 0x3F] : '=');
    out.push_back('=');
  }
  return out;
}

std::string base64_encode(std::string_view data) {
  return base64_encode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool base64_decode(std::string_view text, std::vector<uint8_t>& out) {
  std::array<int, 256> table;
  table.fill(-1);
  for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(kAlphabet[i])] = i;
  out.clear();
  out.reserve((text.size() / 4) * 3);
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    const int v = table[static_cast<uint8_t>(c)];
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(acc >> bits));
    }
  }
  return true;
}

}  // namespace prp
