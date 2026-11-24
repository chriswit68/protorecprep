#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace prp {

// Scales a recorded wall-clock gap into a replay delay. A non-positive gap or
// speed disables pacing; max_gap caps the result when set.
int64_t scaled_delay_ns(int64_t gap_ns, double speed, std::chrono::milliseconds max_gap);

// Owns the monotonic replay schedule derived from recorded timestamps. Kept
// separate from the runner so pacing can be tested without sockets or timers.
class ReplayTimeline {
 public:
  // Establishes or advances the timeline for a record with `ts_ns`. Records
  // without a usable timestamp are scheduled immediately and do not change the
  // baseline. `capture_path` is used for the large-gap warning only.
  void note(int64_t ts_ns, double speed, std::chrono::milliseconds max_gap,
            const std::filesystem::path& capture_path);

  int64_t target_ns() const { return timeline_ns_; }

  // Called after a backpressure retry so a delayed record is not replayed as
  // part of a growing burst.
  void resync(int64_t now_ns);

  // Called after a record is dispatched: if the runner fell behind, the next
  // gap starts from now instead of the stale target.
  void pass_time(int64_t now_ns);

 private:
  bool have_baseline_ = false;
  int64_t last_ts_ns_ = 0;
  int64_t timeline_ns_ = 0;
  bool large_gap_warned_ = false;
  int64_t warned_gap_ns_ = 0;
};

}  // namespace prp
