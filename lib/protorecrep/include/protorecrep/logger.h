#pragma once

#include <string_view>

namespace protorecrep {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Non-owning sink for human-readable log lines and transient progress output.
// The library never writes to stderr/stdout itself; the embedding application
// decides where the output goes. Passing a null ILogger disables all output.
class ILogger {
 public:
  virtual ~ILogger() = default;

  // Whether a message at this level would be emitted. Implementations should
  // return false for levels below their configured threshold.
  virtual bool enabled(LogLevel /*level*/) const { return true; }

  virtual void log(LogLevel level, std::string_view message) = 0;

  // Optional progress reporting. Default implementations are no-ops.
  virtual bool progress_is_tty() const { return false; }
  virtual void progress(std::string_view /*text*/, bool /*in_place*/) {}
  virtual void clear_progress() {}
};

}  // namespace protorecrep
