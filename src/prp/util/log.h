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

inline void log_debug(std::string_view m) { log_message(LogLevel::Debug, m); }
inline void log_info(std::string_view m) { log_message(LogLevel::Info, m); }
inline void log_warn(std::string_view m) { log_message(LogLevel::Warn, m); }
inline void log_error(std::string_view m) { log_message(LogLevel::Error, m); }

}  // namespace prp
