#pragma once

#include <cstdarg>
#include <cstdio>
#include <string_view>
#include <vector>

#include "protorecrep/logger.h"

namespace protorecrep::internal {

// Minimal printf-style logging adapted to an optional ILogger. Keeping this
// helper private lets the library stay free of a concrete logging backend.
inline void logf(ILogger* logger, LogLevel level, const char* fmt, ...) {
  if (logger == nullptr || !logger->enabled(level)) return;

  va_list args;
  va_start(args, fmt);
  va_list again;
  va_copy(again, args);
  const int needed = std::vsnprintf(nullptr, 0, fmt, args);
  va_end(args);
  if (needed < 0) {
    va_end(again);
    return;
  }

  std::vector<char> buffer(static_cast<size_t>(needed) + 1);
  std::vsnprintf(buffer.data(), buffer.size(), fmt, again);
  va_end(again);
  logger->log(level, std::string_view(buffer.data(), static_cast<size_t>(needed)));
}

}  // namespace protorecrep::internal
