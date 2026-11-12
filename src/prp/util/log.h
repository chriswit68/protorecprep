#pragma once

#include <cstdarg>
#include <string>
#include <string_view>

namespace prp {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_log_level(LogLevel level);
LogLevel log_level();
bool log_enabled(LogLevel level);

// Thread-safe, line-buffered logging to stderr.
void log_message(LogLevel level, std::string_view msg);
void log_printf(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Transient progress output on stderr.
// With `in_place` the text is written with a leading '\r' and no trailing
// newline, so one physical line is refreshed; log messages terminate that line
// first, keeping normal logs readable. Without `in_place` the text is written
// as a complete newline-terminated line, which is what line-oriented log
// consumers (containers, CI, redirected files) need to show progress at all.
// Passing an empty string is equivalent to clear_progress_line().
void set_progress_line(std::string_view text, bool in_place);
void clear_progress_line();

// True when stderr is a terminal, i.e. in-place updates are visible.
bool progress_stderr_is_tty();

inline void log_debug(std::string_view m) { log_message(LogLevel::Debug, m); }
inline void log_info(std::string_view m) { log_message(LogLevel::Info, m); }
inline void log_warn(std::string_view m) { log_message(LogLevel::Warn, m); }
inline void log_error(std::string_view m) { log_message(LogLevel::Error, m); }

}  // namespace prp
