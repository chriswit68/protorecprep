#include "prp/replay/timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "prp/lib.h"
#include "prp/util/log.h"

namespace prp {
namespace {

constexpr int64_t kLargeGapWarnNs = 10'000'000'000;  // warn about waits >= 10 s

std::string format_delay(int64_t ns) {
  int64_t seconds = ns / 1'000'000'000;
  if (seconds < 0) seconds = 0;
  const int64_t hours = seconds / 3600;
  seconds %= 3600;
  const int64_t minutes = seconds / 60;
  seconds %= 60;
  char buf[64];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%lldh %lldm %llds", static_cast<long long>(hours),
                  static_cast<long long>(minutes), static_cast<long long>(seconds));
  } else if (minutes > 0) {
    std::snprintf(buf, sizeof(buf), "%lldm %llds", static_cast<long long>(minutes),
                  static_cast<long long>(seconds));
  } else {
    std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(seconds));
  }
  return buf;
}

}  // namespace

int64_t scaled_delay_ns(int64_t gap_ns, double speed, std::chrono::milliseconds max_gap) {
  if (gap_ns <= 0 || speed <= 0.0) return 0;
  const double scaled = static_cast<double>(gap_ns) / speed;
  double capped = scaled;
  if (max_gap.count() > 0) {
    const double cap = static_cast<double>(max_gap.count()) * 1'000'000.0;
    if (capped > cap) capped = cap;
  }
  const double kMax = static_cast<double>(std::numeric_limits<int64_t>::max() / 2);
  if (!std::isfinite(capped) || capped >= kMax) return static_cast<int64_t>(kMax);
  if (capped <= 0.0) return 0;
  return static_cast<int64_t>(capped);
}

void ReplayTimeline::note(int64_t ts_ns, double speed, std::chrono::milliseconds max_gap,
                          const std::filesystem::path& capture_path) {
  const int64_t now = now_monotonic_ns();
  if (!have_baseline_) {
    have_baseline_ = true;
    timeline_ns_ = now;
    // A record without a wall-clock timestamp must not become the baseline.
    last_ts_ns_ = ts_ns > 0 ? ts_ns : 0;
    return;
  }
  if (ts_ns <= 0) return;  // no timestamp, no pacing
  if (last_ts_ns_ <= 0) {
    last_ts_ns_ = ts_ns;  // establish the baseline late
    return;
  }

  int64_t gap = ts_ns - last_ts_ns_;
  last_ts_ns_ = ts_ns;
  if (gap < 0) gap = 0;
  const int64_t delay = scaled_delay_ns(gap, speed, max_gap);
  if (delay >= kLargeGapWarnNs && (!large_gap_warned_ || delay > warned_gap_ns_ * 2)) {
    large_gap_warned_ = true;
    warned_gap_ns_ = delay;
    log_printf(LogLevel::Warn,
               "large recorded gap of %s before %s at speed=%s; the target may close an idle "
               "connection -- consider --max-gap",
               format_delay(delay).c_str(), capture_path.string().c_str(),
               std::to_string(speed).c_str());
  }
  timeline_ns_ += delay;
}

void ReplayTimeline::resync(int64_t now_ns) { timeline_ns_ = std::max(timeline_ns_, now_ns); }

void ReplayTimeline::pass_time(int64_t now_ns) {
  if (now_ns > timeline_ns_) timeline_ns_ = now_ns;
}

}  // namespace prp
