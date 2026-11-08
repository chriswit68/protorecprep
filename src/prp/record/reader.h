#pragma once

#include <cstdint>
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

}  // namespace prp
