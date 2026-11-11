#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "prp/record/format.h"
#include "prp/util/fs.h"

namespace prp {

struct ScanStats {
  bool header_ok = false;
  rec::FileHeader header;
  fs::path path;
  uint64_t file_size = 0;
  uint64_t header_size = 0;
  uint64_t records = 0;
  uint64_t data_records = 0;
  uint64_t data_bytes = 0;
  uint64_t open_records = 0;
  uint64_t close_records = 0;
  uint64_t marker_records = 0;
  bool has_trailer = false;
  rec::TrailerInfo trailer;
  uint64_t valid_end = 0;
  bool truncated = false;
  std::string damage;
};

// Scans a single capture file, validating every record header and CRC.
// Returns false only for fatal problems (cannot open, invalid file header).
// Tail damage (partial record, CRC mismatch) is reported via `stats.truncated`.
bool scan_capture_file(const fs::path& path, ScanStats& stats, std::string& err);

// Scans every capture-*.pcr in a directory in descending order, applying the
// same validation. When `truncate_damaged` is set, damaged tails are cut off
// at the last valid record boundary.
bool recover_session_dir(const fs::path& dir, std::vector<ScanStats>& results, bool truncate_damaged,
                         std::string& err);

// Lists capture-*.pcr files in ascending (chronological) order.
bool list_capture_files(const fs::path& dir, std::vector<fs::path>& files, std::string& err);

// Reads and validates only the file header (magic, version, descriptor, CRC).
bool read_capture_header(const fs::path& path, rec::FileHeader& header, size_t& header_size,
                         std::string& err);

enum class ReadStatus {
  Record,
  Eof,
  Damaged,
  Error,
};

struct CaptureRecord {
  rec::RecordType type = rec::RecordType::Data;
  uint32_t stream_id = 0;
  Direction dir = Direction::In;
  int64_t ts_ns = 0;
  uint8_t close_reason = 0;
  std::string peer;
  std::string type_name;
  std::vector<uint8_t> data;
};

// Sequential, CRC-validating reader for a single capture file. Damaged tails
// are reported once via ReadStatus::Damaged; the following call returns Eof so
// the caller can continue with the next chunk.
class CaptureReader {
 public:
  CaptureReader() = default;
  ~CaptureReader();

  CaptureReader(const CaptureReader&) = delete;
  CaptureReader& operator=(const CaptureReader&) = delete;

  bool open(const fs::path& path, std::string& err);
  ReadStatus next(CaptureRecord& out, std::string& detail);
  void close();

  bool is_open() const { return file_ != nullptr; }
  const rec::FileHeader& header() const { return header_; }
  const fs::path& path() const { return path_; }
  uint64_t records_read() const { return records_read_; }
  // Bytes of the underlying file already consumed: the file header plus every
  // fully validated record. Suitable for a byte-based progress display.
  uint64_t bytes_consumed() const { return bytes_consumed_; }
  uint64_t file_size() const { return file_size_; }

 private:
  FILE* file_ = nullptr;
  fs::path path_;
  rec::FileHeader header_;
  std::vector<uint8_t> payload_;
  bool damaged_ = false;
  uint64_t records_read_ = 0;
  uint64_t bytes_consumed_ = 0;
  uint64_t file_size_ = 0;
};

}  // namespace prp
