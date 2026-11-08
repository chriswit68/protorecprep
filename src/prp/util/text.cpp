#include "prp/util/text.h"

#include <chrono>
#include <cstdio>

namespace prp {

int64_t now_wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int64_t now_monotonic_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  size_t i = 0;
  while (i < text.size()) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c < 0x80) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out.push_back(static_cast<char>(c));
          }
      }
      ++i;
      continue;
    }
    // Validate a UTF-8 sequence; replace invalid bytes defensively.
    int extra = 0;
    if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;
    bool valid = extra > 0 && i + static_cast<size_t>(extra) < text.size();
    for (int k = 1; valid && k <= extra; ++k) {
      const uint8_t cc = static_cast<uint8_t>(text[i + static_cast<size_t>(k)]);
      if ((cc & 0xC0) != 0x80) valid = false;
    }
    if (!valid) {
      out += "\\ufffd";
      ++i;
      continue;
    }
    out.append(text.data() + i, static_cast<size_t>(extra) + 1);
    i += static_cast<size_t>(extra) + 1;
  }
  return out;
}

std::string format_bytes(uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.2f %s", value, kUnits[unit]);
  return buf;
}

}  // namespace prp
