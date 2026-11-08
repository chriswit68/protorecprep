#include "prp/util/sha256.h"

#include <algorithm>
#include <cstring>

namespace prp {
namespace {

constexpr uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256() : state_({{kInit[0], kInit[1], kInit[2], kInit[3], kInit[4], kInit[5], kInit[6], kInit[7]}}) {}

void Sha256::process_block(const uint8_t* block) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  total_len_ += n;
  if (buffer_len_ > 0) {
    const size_t take = std::min(n, 64 - buffer_len_);
    std::memcpy(buffer_.data() + buffer_len_, p, take);
    buffer_len_ += take;
    p += take;
    n -= take;
    if (buffer_len_ == 64) {
      process_block(buffer_.data());
      buffer_len_ = 0;
    }
  }
  while (n >= 64) {
    process_block(p);
    p += 64;
    n -= 64;
  }
  if (n > 0) {
    std::memcpy(buffer_.data(), p, n);
    buffer_len_ = n;
  }
}

Sha256Digest Sha256::finish() {
  if (!finished_) {
    const uint64_t bit_len = total_len_ * 8u;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (buffer_len_ != 56) update(&zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; ++i) len_bytes[i] = static_cast<uint8_t>(bit_len >> (56 - 8 * i));
    update(len_bytes, 8);
    finished_ = true;
  }
  Sha256Digest out{};
  for (int i = 0; i < 8; ++i) {
    const size_t idx = static_cast<size_t>(i);
    const size_t base = idx * 4;
    out[base] = static_cast<uint8_t>(state_[idx] >> 24);
    out[base + 1] = static_cast<uint8_t>(state_[idx] >> 16);
    out[base + 2] = static_cast<uint8_t>(state_[idx] >> 8);
    out[base + 3] = static_cast<uint8_t>(state_[idx]);
  }
  return out;
}

Sha256Digest Sha256::digest(std::string_view data) {
  Sha256 h;
  h.update(data.data(), data.size());
  return h.finish();
}

std::string Sha256::hex(const Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (uint8_t b : digest) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

std::string Sha256::hex_of(std::string_view data) { return hex(digest(data)); }

}  // namespace prp
