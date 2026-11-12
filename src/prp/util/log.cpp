#include "prp/util/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#include <unistd.h>

namespace prp {
namespace {

std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;
bool g_progress_active = false;

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
  }
  return "?????";
}

}  // namespace

void set_log_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }
LogLevel log_level() { return g_level.load(std::memory_order_relaxed); }
bool log_enabled(LogLevel level) {
  return static_cast<int>(level) >= static_cast<int>(g_level.load(std::memory_order_relaxed));
}

void log_message(LogLevel level, std::string_view msg) {
  if (!log_enabled(level)) return;

  const auto now = std::chrono::system_clock::now();
  const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);

  std::tm tm{};
  localtime_r(&t, &tm);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_progress_active) {
    std::fputc('\n', stderr);
    g_progress_active = false;
  }
  std::fprintf(stderr, "[%s.%03lld] %s %.*s\n", stamp, static_cast<long long>(ms), level_name(level),
               static_cast<int>(msg.size()), msg.data());
  std::fflush(stderr);
}

void log_printf(LogLevel level, const char* fmt, ...) {
  if (!log_enabled(level)) return;
  char stack_buf[512];
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
  va_end(args);
  if (needed < 0) {
    va_end(copy);
    return;
  }
  if (static_cast<size_t>(needed) < sizeof(stack_buf)) {
    va_end(copy);
    log_message(level, std::string_view(stack_buf, static_cast<size_t>(needed)));
    return;
  }
  std::string heap(static_cast<size_t>(needed), '\0');
  std::vsnprintf(heap.data(), heap.size() + 1, fmt, copy);
  va_end(copy);
  log_message(level, heap);
}

void clear_progress_line() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_progress_active) return;
  std::fputc('\n', stderr);
  std::fflush(stderr);
  g_progress_active = false;
}

void set_progress_line(std::string_view text, bool in_place) {
  if (text.empty()) {
    clear_progress_line();
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (in_place) {
    std::fprintf(stderr, "\r%.*s", static_cast<int>(text.size()), text.data());
    g_progress_active = true;
  } else {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(text.size()), text.data());
    g_progress_active = false;
  }
  std::fflush(stderr);
}

bool progress_stderr_is_tty() {
  static const bool tty = ::isatty(::fileno(stderr)) != 0;
  return tty;
}

}  // namespace prp
