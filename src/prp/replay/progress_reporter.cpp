#include "prp/replay/progress_reporter.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "prp/lib.h"
#include "prp/util/log.h"

namespace prp {
namespace {

constexpr int64_t kInlineIntervalNs = 100'000'000;  // in-place refresh: at most every 100 ms
constexpr int64_t kLineIntervalNs = 1'000'000'000;  // line output: at most every 1 s

std::string prefix_for(size_t file_index, size_t file_count) {
  if (file_count <= 1) return {};
  return "[" + std::to_string(file_index + 1) + "/" + std::to_string(file_count) + "] ";
}

}  // namespace

ProgressReporter::ProgressReporter(ProgressMode mode) {
  switch (mode) {
    case ProgressMode::Off:
      enabled_ = false;
      break;
    case ProgressMode::Line:
      enabled_ = true;
      inline_progress_ = false;
      break;
    case ProgressMode::Inline:
      enabled_ = true;
      inline_progress_ = true;
      break;
    case ProgressMode::Auto:
      enabled_ = true;
      inline_progress_ = progress_stderr_is_tty();
      break;
  }
}

void ProgressReporter::begin(uint64_t consumed, uint64_t size, const std::filesystem::path& path,
                             size_t file_index, size_t file_count) {
  last_percent_ = -1;
  last_ns_ = 0;
  update(consumed, size, path, file_index, file_count, true);
}

void ProgressReporter::update(uint64_t consumed, uint64_t size, const std::filesystem::path& path,
                              size_t file_index, size_t file_count, bool force) {
  if (!enabled_) return;
  unsigned percent = 100;
  if (size > 0) {
    const uint64_t capped = std::min(consumed, size);
    percent = static_cast<unsigned>(capped * 100 / size);
    if (percent > 100) percent = 100;
  }
  const int64_t now = now_monotonic_ns();
  if (!force) {
    if (static_cast<int>(percent) == last_percent_) return;
    const int64_t interval = inline_progress_ ? kInlineIntervalNs : kLineIntervalNs;
    if (last_ns_ != 0 && now - last_ns_ < interval) return;
  }
  last_percent_ = static_cast<int>(percent);
  last_ns_ = now;
  char tail[16];
  std::snprintf(tail, sizeof(tail), "%3u%%", percent);
  set_progress_line(prefix_for(file_index, file_count) + path.string() + " " + tail, inline_progress_);
}

void ProgressReporter::finish(uint64_t consumed, uint64_t size, const std::filesystem::path& path,
                              size_t file_index, size_t file_count) {
  if (enabled_) update(consumed, size, path, file_index, file_count, true);
  clear_progress_line();
}

void ProgressReporter::clear() { clear_progress_line(); }

}  // namespace prp
