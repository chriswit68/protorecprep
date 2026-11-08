#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace prp {

namespace fs = std::filesystem;

// Creates the directory and all parents. Returns false and fills `err` on
// failure (never throws).
bool ensure_directory(const fs::path& dir, std::string& err);

// Writes `content` to `path` atomically (temp file + rename), fsyncing the
// file and the containing directory.
bool write_file_atomic(const fs::path& path, std::string_view content, std::string& err);

bool read_file(const fs::path& path, std::string& out, std::string& err);

uint64_t file_size(const fs::path& path);

// Checks free space on the filesystem containing `path`.
bool free_space(const fs::path& path, uint64_t& free_bytes, uint64_t& total_bytes);

std::string random_hex(size_t bytes);
std::string hostname();
std::string utc_timestamp_compact();
std::string iso8601_now();

}  // namespace prp
