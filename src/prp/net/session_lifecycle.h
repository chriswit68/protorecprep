#pragma once

namespace prp {

// Lifecycle of one accepted connection. All access happens on the session's
// Asio strand, so transitions are explicit rather than synchronized. Keeping
// them in one place prevents combinations such as "paused and closing" from
// being set by unrelated handlers.
class SessionLifecycle {
 public:
  bool started() const { return started_; }
  void mark_started() { started_ = true; }

  bool closing() const { return closing_; }
  // Returns false when the session was already closing.
  bool begin_close() {
    if (closing_) return false;
    closing_ = true;
    return true;
  }

  bool paused() const { return paused_; }
  // Returns false when reads were not running and therefore cannot be paused.
  bool pause_reads() {
    if (closing_ || paused_) return false;
    paused_ = true;
    return true;
  }
  void resume_reads() { paused_ = false; }

  bool finalized() const { return finalized_; }
  // Returns true exactly once, for the handler that performs the final cleanup.
  bool finalize() {
    if (finalized_) return false;
    finalized_ = true;
    return true;
  }

 private:
  bool started_ = false;
  bool closing_ = false;
  bool paused_ = false;
  bool finalized_ = false;
};

}  // namespace prp
