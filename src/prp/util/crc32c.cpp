#include "prp/util/crc32c.h"

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#include <array>

namespace prp {
namespace {

#if !defined(__ARM_FEATURE_CRC32)
constexpr uint32_t kPoly = 0x82F63B78u;  // reflected Castagnoli polynomial

struct Tables {
  std::array<std::array<uint32_t, 256>, 8> t{};
  constexpr Tables() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1u) ? kPoly : 0u);
      t[0][i] = crc;
    }
    for (uint32_t i = 0; i < 256; ++i) {
      for (int s = 1; s < 8; ++s) {
        t[static_cast<size_t>(s)][i] =
            (t[static_cast<size_t>(s - 1)][i] >> 8) ^ t[0][t[static_cast<size_t>(s - 1)][i] & 0xFFu];
      }
    }
  }
};

constexpr Tables kTables{};

uint32_t crc32c_sw(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n >= 8) {
    const uint32_t low = crc ^ (static_cast<uint32_t>(p[0]) |
                                (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
    crc = kTables.t[7][low & 0xFFu] ^ kTables.t[6][(low >> 8) & 0xFFu] ^
          kTables.t[5][(low >> 16) & 0xFFu] ^ kTables.t[4][(low >> 24) & 0xFFu] ^
          kTables.t[3][p[4]] ^ kTables.t[2][p[5]] ^ kTables.t[1][p[6]] ^ kTables.t[0][p[7]];
    p += 8;
    n -= 8;
  }
  while (n--) crc = (crc >> 8) ^ kTables.t[0][(crc ^ *p++) & 0xFFu];
  return ~crc;
}
#endif

}  // namespace

uint32_t crc32c_extend(uint32_t crc, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
#if defined(__ARM_FEATURE_CRC32)
  crc = ~crc;
  while (n >= 8) {
    uint64_t word;
    __builtin_memcpy(&word, p, sizeof(word));
    crc = __crc32cd(crc, word);
    p += 8;
    n -= 8;
  }
  while (n--) crc = __crc32cb(crc, *p++);
  return ~crc;
#else
  return crc32c_sw(crc, p, n);
#endif
}

uint32_t crc32c(const void* data, size_t n, uint32_t seed) { return crc32c_extend(seed, data, n); }

}  // namespace prp
