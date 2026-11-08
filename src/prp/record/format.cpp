#include "prp/record/format.h"

#include <cstring>

#include "prp/util/bytes.h"
#include "prp/util/crc32c.h"

namespace prp::rec {
namespace {

constexpr size_t kDataFixed = 16;     // stream_id + dir + pad + ts
constexpr size_t kTrailerFixed = 28;  // records + bytes + clean + pad + ts

void append_i64le(std::vector<uint8_t>& out, int64_t v) {
  append_u64le(out, static_cast<uint64_t>(v));
}
int64_t get_i64le(const uint8_t* p) { return static_cast<int64_t>(get_u64le(p)); }

}  // namespace

const char* framing_name(Framing framing) {
  switch (framing) {
    case Framing::Raw: return "raw";
    case Framing::VarintLength: return "varint-length";
  }
  return "unknown";
}

bool parse_framing(std::string_view text, Framing& out) {
  if (text == "raw") {
    out = Framing::Raw;
    return true;
  }
  if (text == "varint" || text == "varint-length" || text == "length-prefixed") {
    out = Framing::VarintLength;
    return true;
  }
  return false;
}

const char* record_type_name(RecordType type) {
  switch (type) {
    case RecordType::StreamOpen: return "stream_open";
    case RecordType::Data: return "data";
    case RecordType::StreamClose: return "stream_close";
    case RecordType::Marker: return "marker";
    case RecordType::Trailer: return "trailer";
  }
  return "unknown";
}

std::vector<uint8_t> encode_file_header(const FileHeader& header) {
  const uint32_t desc_len = static_cast<uint32_t>(header.descriptor_set.size());
  const uint32_t header_size = static_cast<uint32_t>(kFileHeaderFixedSize + desc_len + 4);

  std::vector<uint8_t> out;
  out.reserve(header_size);
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  append_u16le(out, header.version);
  append_u16le(out, header.flags);
  append_u32le(out, header_size);
  out.insert(out.end(), header.session_uuid.begin(), header.session_uuid.end());
  out.push_back(static_cast<uint8_t>(header.framing));
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  append_i64le(out, header.start_wall_ns);
  out.insert(out.end(), header.proto_sha256.begin(), header.proto_sha256.end());
  append_u32le(out, desc_len);
  out.insert(out.end(), header.descriptor_set.begin(), header.descriptor_set.end());
  const uint32_t crc = crc32c(out.data(), out.size());
  append_u32le(out, crc);
  return out;
}

bool decode_file_header(const uint8_t* data, size_t size, FileHeader& out, size_t& header_size, std::string& err) {
  if (size < kFileHeaderFixedSize + 4) {
    err = "file too small for header";
    return false;
  }
  if (std::memcmp(data, kMagic.data(), kMagic.size()) != 0) {
    err = "bad magic (not a .pcr capture)";
    return false;
  }
  const uint16_t version = get_u16le(data + 4);
  if (version == 0 || version > kFormatVersion) {
    err = "unsupported format version " + std::to_string(version);
    return false;
  }
  const uint32_t total = get_u32le(data + 8);
  if (total < kFileHeaderFixedSize + 4 || total > size) {
    err = "invalid header size";
    return false;
  }
  const uint8_t framing_byte = data[28];
  if (framing_byte > 1) {
    err = "invalid framing byte in header";
    return false;
  }
  const uint32_t desc_len = get_u32le(data + 72);
  if (desc_len > kMaxDescriptorBytes || kFileHeaderFixedSize + desc_len + 4 != total) {
    err = "invalid descriptor length in header";
    return false;
  }
  const uint32_t stored_crc = get_u32le(data + total - 4);
  const uint32_t actual_crc = crc32c(data, total - 4);
  if (stored_crc != actual_crc) {
    err = "header CRC mismatch";
    return false;
  }

  out.version = version;
  out.flags = get_u16le(data + 6);
  std::memcpy(out.session_uuid.data(), data + 12, out.session_uuid.size());
  out.framing = static_cast<Framing>(framing_byte);
  out.start_wall_ns = get_i64le(data + 32);
  std::memcpy(out.proto_sha256.data(), data + 40, out.proto_sha256.size());
  out.descriptor_set.assign(reinterpret_cast<const char*>(data + 76), desc_len);
  header_size = total;
  return true;
}

std::array<uint8_t, kRecordPrefixSize> encode_record_prefix(RecordType type, uint32_t payload_len, uint32_t crc) {
  std::array<uint8_t, kRecordPrefixSize> out{};
  out[0] = static_cast<uint8_t>(type);
  put_u32le(out.data() + 1, payload_len);
  put_u32le(out.data() + 5, crc);
  return out;
}

void append_stream_open_payload(std::vector<uint8_t>& out, uint32_t stream_id, int64_t ts_ns,
                                std::string_view peer, std::string_view type_name) {
  if (peer.size() > 0xFFFF) peer = peer.substr(0, 0xFFFF);
  if (type_name.size() > 0xFFFF) type_name = type_name.substr(0, 0xFFFF);
  append_u32le(out, stream_id);
  append_i64le(out, ts_ns);
  append_u16le(out, static_cast<uint16_t>(peer.size()));
  append_u16le(out, static_cast<uint16_t>(type_name.size()));
  append_bytes(out, peer.data(), peer.size());
  append_bytes(out, type_name.data(), type_name.size());
}

void append_data_payload(std::vector<uint8_t>& out, uint32_t stream_id, Direction dir, int64_t ts_ns,
                         const void* data, size_t n) {
  append_u32le(out, stream_id);
  out.push_back(static_cast<uint8_t>(dir));
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  append_i64le(out, ts_ns);
  append_bytes(out, data, n);
}

void append_stream_close_payload(std::vector<uint8_t>& out, uint32_t stream_id, CloseReason reason,
                                 int64_t ts_ns) {
  append_u32le(out, stream_id);
  out.push_back(static_cast<uint8_t>(reason));
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  append_i64le(out, ts_ns);
}

void append_marker_payload(std::vector<uint8_t>& out, int64_t ts_ns, std::string_view text) {
  append_i64le(out, ts_ns);
  append_bytes(out, text.data(), text.size());
}

void append_trailer_payload(std::vector<uint8_t>& out, uint64_t records, uint64_t bytes, bool clean,
                            int64_t ts_ns) {
  append_u64le(out, records);
  append_u64le(out, bytes);
  out.push_back(clean ? 1 : 0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  append_i64le(out, ts_ns);
}

bool parse_trailer(const uint8_t* payload, size_t len, TrailerInfo& out) {
  if (len < kTrailerFixed) return false;
  out.records = get_u64le(payload);
  out.bytes = get_u64le(payload + 8);
  out.clean = payload[16] != 0;
  out.end_wall_ns = get_i64le(payload + 20);
  return true;
}

size_t data_payload_bytes(const uint8_t* payload, size_t len) {
  (void)payload;
  return len > kDataFixed ? len - kDataFixed : 0;
}

}  // namespace prp::rec
