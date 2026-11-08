#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prp/core/capture_item.h"
#include "prp/util/sha256.h"

namespace prp::rec {

inline constexpr std::array<uint8_t, 4> kMagic{{'P', 'C', 'R', 'P'}};
inline constexpr uint16_t kFormatVersion = 1;
inline constexpr uint32_t kMaxDescriptorBytes = 16u * 1024u * 1024u;
inline constexpr uint32_t kMaxRecordPayload = 64u * 1024u * 1024u;
inline constexpr size_t kFileHeaderFixedSize = 76;
inline constexpr size_t kRecordPrefixSize = 9;

enum class Framing : uint8_t { Raw = 0, VarintLength = 1 };

const char* framing_name(Framing framing);
bool parse_framing(std::string_view text, Framing& out);

enum class RecordType : uint8_t {
  StreamOpen = 1,
  Data = 2,
  StreamClose = 3,
  Marker = 4,
  Trailer = 5,
};

const char* record_type_name(RecordType type);

struct FileHeader {
  uint16_t version = kFormatVersion;
  uint16_t flags = 0;
  std::array<uint8_t, 16> session_uuid{};
  Framing framing = Framing::VarintLength;
  int64_t start_wall_ns = 0;
  Sha256Digest proto_sha256{};
  std::string descriptor_set;  // serialized google.protobuf.FileDescriptorSet
};

std::vector<uint8_t> encode_file_header(const FileHeader& header);
bool decode_file_header(const uint8_t* data, size_t size, FileHeader& out, size_t& header_size, std::string& err);

std::array<uint8_t, kRecordPrefixSize> encode_record_prefix(RecordType type, uint32_t payload_len, uint32_t crc);

void append_stream_open_payload(std::vector<uint8_t>& out, uint32_t stream_id, int64_t ts_ns, std::string_view peer,
                                std::string_view type_name);
void append_data_payload(std::vector<uint8_t>& out, uint32_t stream_id, Direction dir, int64_t ts_ns,
                         const void* data, size_t n);
void append_stream_close_payload(std::vector<uint8_t>& out, uint32_t stream_id, CloseReason reason, int64_t ts_ns);
void append_marker_payload(std::vector<uint8_t>& out, int64_t ts_ns, std::string_view text);
void append_trailer_payload(std::vector<uint8_t>& out, uint64_t records, uint64_t bytes, bool clean, int64_t ts_ns);

struct TrailerInfo {
  uint64_t records = 0;
  uint64_t bytes = 0;
  bool clean = false;
  int64_t end_wall_ns = 0;
};
bool parse_trailer(const uint8_t* payload, size_t len, TrailerInfo& out);
size_t data_payload_bytes(const uint8_t* payload, size_t len);

}  // namespace prp::rec
