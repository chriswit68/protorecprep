#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace prp {

// Result of inspecting a blob payload from a @file/@blob typed frame.
// `data` points into the caller's payload after the optional u32 LE prefix has
// been stripped, so it stays valid only as long as that payload does.
struct BlobInfo {
  std::string_view data;
  std::string kind;       // pcap, pcapng, wav, png, jpeg, gif, pdf, zip, gzip, zstd, text, unknown
  std::string extension;  // file extension without the dot
  bool has_prefix = false;
  uint32_t prefix = 0;
};

BlobInfo analyze_blob(std::string_view payload);

}  // namespace prp
