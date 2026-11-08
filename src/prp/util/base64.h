#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prp {

std::string base64_encode(std::string_view data);
std::string base64_encode(const uint8_t* data, size_t n);
bool base64_decode(std::string_view text, std::vector<uint8_t>& out);

}  // namespace prp
