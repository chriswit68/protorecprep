#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace prp {

int64_t now_wall_ns();
int64_t now_monotonic_ns();

// Escapes a string for inclusion in a JSON document (without surrounding
// quotes). Invalid UTF-8 bytes are replaced with U+FFFD.
std::string json_escape(std::string_view text);

std::string format_bytes(uint64_t bytes);

}  // namespace prp
