#include "protorecrep/reader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "protorecrep/util/bytes.h"
#include "protorecrep/util/crc32c.h"

namespace protorecrep {
namespace {

bool read_exact(FILE* f, void* dst, size_t n, size_t& got) {
  got = std::fread(dst, 1, n, f);
  return got == n || (got < n && std::feof(f));
}

// Reads and validates a file header from an already-open stream.
bool read_file_header(FILE* f, rec::FileHeader& header, size_t& header_size, std::string& err) {
  uint8_t fixed[rec::kFileHeaderFixedSize];
  size_t got = 0;
  if (!read_exact(f, fixed, sizeof(fixed), got) || got < sizeof(fixed)) {
    err = "file too small for header";
    return false;
  }
  if (std::memcmp(fixed, rec::kMagic.data(), rec::kMagic.size()) != 0) {
    err = "bad magic (not a .pcr capture)";
    return false;
  }
  const uint32_t header_total = get_u32le(fixed + 8);
  if (header_total < rec::kFileHeaderFixedSize + 4 || header_total > 16u * 1024u * 1024u) {
    err = "invalid header size";
    return false;
  }
  std::vector<uint8_t> header_buf(header_total);
  std::memcpy(header_buf.data(), fixed, sizeof(fixed));
  if (header_total > sizeof(fixed)) {
    const size_t remaining = header_total - sizeof(fixed);
    if (!read_exact(f, header_buf.data() + sizeof(fixed), remaining, got) || got < remaining) {
      err = "truncated file header";
      return false;
    }
  }
  size_t consumed = 0;
  if (!rec::decode_file_header(header_buf.data(), header_buf.size(), header, consumed, err)) return false;
  header_size = consumed;
  return true;
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
  // Rewind and let the shared header reader validate magic/size/CRC.
  if (std::fseek(f, 0, SEEK_SET) != 0) {
    std::fclose(f);
    err = "cannot rewind capture file";
    return false;
  }
  size_t header_size = 0;
  if (!read_file_header(f, stats.header, header_size, err)) {
    std::fclose(f);
    return false;
  }
  stats.header_ok = true;
  stats.header_size = header_size;
  stats.valid_end = header_size;

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

bool list_capture_files(const fs::path& dir, std::vector<fs::path>& files, std::string& err) {
  files.clear();
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (ec) {
      err = "cannot access " + dir.string() + ": " + ec.message();
    } else {
      err = dir.string() + " is not a directory";
    }
    return false;
  }
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("capture-", 0) == 0 && entry.path().extension() == ".pcr") {
      files.push_back(entry.path());
    }
  }
  if (ec) {
    err = "cannot list " + dir.string() + ": " + ec.message();
    return false;
  }
  std::sort(files.begin(), files.end());
  return true;
}

bool read_capture_header(const fs::path& path, rec::FileHeader& header, size_t& header_size,
                         std::string& err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = "cannot open " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  const bool ok = read_file_header(f, header, header_size, err);
  std::fclose(f);
  return ok;
}

bool recover_session_dir(const fs::path& dir, std::vector<ScanStats>& results, bool truncate_damaged,
                         std::string& err) {
  results.clear();
  std::vector<fs::path> files;
  if (!list_capture_files(dir, files, err)) return false;
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

CaptureReader::~CaptureReader() { close(); }

void CaptureReader::close() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool CaptureReader::open(const fs::path& path, std::string& err) {
  close();
  path_ = path;
  damaged_ = false;
  records_read_ = 0;
  bytes_consumed_ = 0;
  file_size_ = 0;
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) {
    err = "cannot open " + path.string() + ": " + std::strerror(errno);
    return false;
  }
  struct stat st{};
  if (::fstat(::fileno(file_), &st) == 0) file_size_ = static_cast<uint64_t>(st.st_size);
  size_t header_size = 0;
  if (!read_file_header(file_, header_, header_size, err)) {
    close();
    err = path.string() + ": " + err;
    return false;
  }
  bytes_consumed_ = header_size;
  return true;
}

ReadStatus CaptureReader::next(CaptureRecord& out, std::string& detail) {
  if (!file_ || damaged_) return ReadStatus::Eof;

  out = CaptureRecord{};
  std::array<uint8_t, rec::kRecordPrefixSize> prefix{};
  size_t got = 0;
  if (!read_exact(file_, prefix.data(), prefix.size(), got)) {
    detail = "read error after " + std::to_string(records_read_) + " record(s)";
    damaged_ = true;
    return ReadStatus::Error;
  }
  if (got == 0) {
    // Clean EOF: the whole file has been consumed.
    bytes_consumed_ = file_size_;
    return ReadStatus::Eof;
  }
  if (got < prefix.size()) {
    detail = "partial record prefix";
    damaged_ = true;
    return ReadStatus::Damaged;
  }

  const uint8_t type_byte = prefix[0];
  if (type_byte < static_cast<uint8_t>(rec::RecordType::StreamOpen) ||
      type_byte > static_cast<uint8_t>(rec::RecordType::Trailer)) {
    detail = "invalid record type";
    damaged_ = true;
    return ReadStatus::Damaged;
  }
  const uint32_t len = get_u32le(prefix.data() + 1);
  if (len > rec::kMaxRecordPayload) {
    detail = "record length exceeds limit";
    damaged_ = true;
    return ReadStatus::Damaged;
  }
  payload_.resize(len);
  got = 0;
  if (len > 0 && (!read_exact(file_, payload_.data(), len, got) || got < len)) {
    if (std::ferror(file_)) {
      detail = "read error in record payload";
      damaged_ = true;
      return ReadStatus::Error;
    }
    detail = "partial record payload";
    damaged_ = true;
    return ReadStatus::Damaged;
  }
  const uint32_t stored_crc = get_u32le(prefix.data() + 5);
  if (crc32c(payload_.data(), payload_.size()) != stored_crc) {
    detail = "record CRC mismatch";
    damaged_ = true;
    return ReadStatus::Damaged;
  }

  const auto type = static_cast<rec::RecordType>(type_byte);
  switch (type) {
    case rec::RecordType::StreamOpen: {
      rec::StreamOpenInfo info;
      if (!rec::parse_stream_open(payload_.data(), payload_.size(), info)) {
        detail = "invalid stream_open payload";
        damaged_ = true;
        return ReadStatus::Damaged;
      }
      out.stream_id = info.stream_id;
      out.ts_ns = info.ts_ns;
      out.peer = std::move(info.peer);
      out.type_name = std::move(info.type_name);
      break;
    }
    case rec::RecordType::Data: {
      rec::DataInfo info;
      if (!rec::parse_data(payload_.data(), payload_.size(), info)) {
        detail = "invalid data payload";
        damaged_ = true;
        return ReadStatus::Damaged;
      }
      out.stream_id = info.stream_id;
      out.dir = info.dir;
      out.ts_ns = info.ts_ns;
      out.data.assign(info.data, info.data + info.size);
      break;
    }
    case rec::RecordType::StreamClose: {
      rec::StreamCloseInfo info;
      if (!rec::parse_stream_close(payload_.data(), payload_.size(), info)) {
        detail = "invalid stream_close payload";
        damaged_ = true;
        return ReadStatus::Damaged;
      }
      out.stream_id = info.stream_id;
      out.close_reason = static_cast<uint8_t>(info.reason);
      out.ts_ns = info.ts_ns;
      break;
    }
    case rec::RecordType::Marker: {
      if (payload_.size() < 8) {
        detail = "invalid marker payload";
        damaged_ = true;
        return ReadStatus::Damaged;
      }
      out.ts_ns = static_cast<int64_t>(get_u64le(payload_.data()));
      break;
    }
    case rec::RecordType::Trailer: {
      // The trailer carries the chunk end timestamp. Leaving ts_ns at zero used
      // to reset the replay timeline, so the next chunk's first record looked
      // like a multi-decade gap (and stalled paced multi-chunk replays).
      rec::TrailerInfo info;
      if (rec::parse_trailer(payload_.data(), payload_.size(), info)) out.ts_ns = info.end_wall_ns;
      break;
    }
  }

  out.type = type;
  ++records_read_;
  bytes_consumed_ += rec::kRecordPrefixSize + len;
  return ReadStatus::Record;
}

}  // namespace protorecrep
