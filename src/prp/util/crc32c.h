#pragma once

#include <cstddef>
#include <cstdint>

namespace prp {

// CRC32C (Castagnoli). Hardware accelerated on ARMv8 (CRC extension), fast
// software fallback (slice-by-8) everywhere else.
uint32_t crc32c(const void* data, size_t n, uint32_t seed = 0);

// Streaming variant: start with seed=0, feed chunks, finalize with the return
// value of the last call.
uint32_t crc32c_extend(uint32_t crc, const void* data, size_t n);

}  // namespace prp
