#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include "prp/cli/options.h"

namespace prp {

// Owns the replay progress display: mode resolution, throttling and the
// terminal/line-oriented output decision. The runner only supplies the current
// reader position and chunk number.
class ProgressReporter {
 public:
  explicit ProgressReporter(ProgressMode mode);

  // Resets the throttle and shows the first update for a freshly opened file.
  void begin(uint64_t consumed, uint64_t size, const std::filesystem::path& path, size_t file_index,
             size_t file_count);

  // Refreshes the display when the percentage changed and the interval elapsed.
  // `force` bypasses both checks (used for the first and last update).
  void update(uint64_t consumed, uint64_t size, const std::filesystem::path& path, size_t file_index,
              size_t file_count, bool force = false);

  // Shows the final 100% (if enabled) and terminates an in-place line.
  void finish(uint64_t consumed, uint64_t size, const std::filesystem::path& path, size_t file_index,
              size_t file_count);

  void clear();

 private:
  bool enabled_ = true;
  bool inline_progress_ = true;
  int last_percent_ = -1;
  int64_t last_ns_ = 0;
};

}  // namespace prp
