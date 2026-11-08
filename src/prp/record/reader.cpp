#include "prp/record/reader.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "prp/util/bytes.h"
#include "prp/util/crc32c.h"

namespace prp {
namespace {

bool read_exact(FILE* f, void* dst, size_t n, size_t& got) {
  got = std::fread(dst, 1, n, f);
  return got == n || (got < n && std::feof(f));
}

}  // namespace

bool scan_capture_file(const fs::path& path, ScanStats& stats, std::string& err) {
  stats = ScanStats{};
  stats.path = path;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = "cannot open " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  struct stat st{};
  if (::fstat(::fileno(f), &st) == 0) stats.file_size = static_cast<uint64_t>(st.st_size);

  uint8_t fixed[rec::kFileHeaderFixedSize];
  size_t got = 0;
  if (!read_exact(f, fixed, sizeof(fixed), got) || got < sizeof(fixed)) {
    std::fclose(f);
    err = "file too small for header";
    return false;
  }
  if (std::memcmp(fixed, rec::kMagic.data(), rec::kMagic.size()) != 0) {
    std::fclose(f);
    err = "bad magic (not a .pcr capture)";
    return false;
  }
  const uint32_t header_total = get_u32le(fixed + 8);
  if (header_total < rec::kFileHeaderFixedSize + 4 || header_total > 16u * 1024u * 1024u) {
    std::fclose(f);
    err = "invalid header size";
    return false;
  }
  std::vector<uint8_t> header_buf(header_total);
  std::memcpy(header_buf.data(), fixed, sizeof(fixed));
  if (header_total > sizeof(fixed)) {
    const size_t remaining = header_total - sizeof(fixed);
    if (!read_exact(f, header_buf.data() + sizeof(fixed), remaining, got) || got < remaining) {
      std::fclose(f);
      err = "truncated file header";
      return false;
    }
  }
  size_t consumed = 0;
  if (!rec::decode_file_header(header_buf.data(), header_buf.size(), stats.header, consumed, err)) {
    std::fclose(f);
    return false;
  }
  stats.header_ok = true;
  stats.header_size = consumed;
  stats.valid_end = consumed;

  std::vector<uint8_t> prefix(rec::kRecordPrefixSize);
  std::vector<uint8_t> payload;

  while (true) {
    const long prefix_pos = std::ftell(f);
    if (prefix_pos < 0) break;
    got = 0;
    if (!read_exact(f, prefix.data(), prefix.size(), got)) break;
    if (got == 0) break;  // clean EOF
    if (got < prefix.size()) {
      stats.truncated = true;
      stats.damage = "partial record prefix";
      break;
    }
    const uint8_t type_byte = prefix[0];
    if (type_byte < static_cast<uint8_t>(rec::RecordType::StreamOpen) ||
        type_byte > static_cast<uint8_t>(rec::RecordType::Trailer)) {
      stats.truncated = true;
      stats.damage = "invalid record type";
      break;
    }
    const uint32_t len = get_u32le(prefix.data() + 1);
    if (len > rec::kMaxRecordPayload) {
      stats.truncated = true;
      stats.damage = "record length exceeds limit";
      break;
    }
    payload.resize(len);
    got = 0;
    if (len > 0 && (!read_exact(f, payload.data(), len, got) || got < len)) {
      stats.truncated = true;
      stats.damage = "partial record payload";
      break;
    }
    const uint32_t stored_crc = get_u32le(prefix.data() + 5);
    if (crc32c(payload.data(), payload.size()) != stored_crc) {
      stats.truncated = true;
      stats.damage = "record CRC mismatch";
      break;
    }
    const auto type = static_cast<rec::RecordType>(type_byte);
    ++stats.records;
    switch (type) {
      case rec::RecordType::Data:
        ++stats.data_records;
        stats.data_bytes += rec::data_payload_bytes(payload.data(), payload.size());
        break;
      case rec::RecordType::StreamOpen:
        ++stats.open_records;
        break;
      case rec::RecordType::StreamClose:
        ++stats.close_records;
        break;
      case rec::RecordType::Marker:
        ++stats.marker_records;
        break;
      case rec::RecordType::Trailer:
        stats.has_trailer = rec::parse_trailer(payload.data(), payload.size(), stats.trailer);
        break;
    }
    stats.valid_end = static_cast<uint64_t>(std::ftell(f));
  }

  std::fclose(f);
  return true;
}

bool recover_session_dir(const fs::path& dir, std::vector<ScanStats>& results, bool truncate_damaged,
                         std::string& err) {
  results.clear();
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    err = dir.string() + " is not a directory";
    return false;
  }
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("capture-", 0) == 0 && entry.path().extension() == ".pcr") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& path : files) {
    ScanStats stats;
    std::string scan_err;
    if (!scan_capture_file(path, stats, scan_err)) {
      err = scan_err;
      return false;
    }
    if (stats.truncated && truncate_damaged) {
      if (::truncate(path.c_str(), static_cast<off_t>(stats.valid_end)) != 0) {
        err = "truncate " + path.string() + " failed: " + std::strerror(errno);
        return false;
      }
    }
    results.push_back(std::move(stats));
  }
  return true;
}

}  // namespace prp
