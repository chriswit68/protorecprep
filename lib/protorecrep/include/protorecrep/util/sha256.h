#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace protorecrep {

using Sha256Digest = std::array<uint8_t, 32>;

// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t n);
  Sha256Digest finish();

  static Sha256Digest digest(std::string_view data);
  static std::string hex(const Sha256Digest& digest);
  static std::string hex_of(std::string_view data);

 private:
  void process_block(const uint8_t* block);

  std::array<uint32_t, 8> state_{};
  std::array<uint8_t, 64> buffer_{};
  size_t buffer_len_ = 0;
  uint64_t total_len_ = 0;
  bool finished_ = false;
};

}  // namespace protorecrep
