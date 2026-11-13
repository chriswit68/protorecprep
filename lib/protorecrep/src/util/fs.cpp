#include "protorecrep/util/fs.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <system_error>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

namespace protorecrep {

bool ensure_directory(const fs::path& dir, std::string& err) {
  std::error_code ec;
  if (fs::exists(dir, ec)) {
    if (fs::is_directory(dir, ec)) return true;
    err = dir.string() + " exists but is not a directory";
    return false;
  }
  fs::create_directories(dir, ec);
  if (ec) {
    err = "cannot create directory " + dir.string() + ": " + ec.message();
    return false;
  }
  return true;
}

fs::path resolve_subdir(const fs::path& base, const fs::path& sub) {
  if (sub.empty()) return base;
  if (sub.is_absolute()) return sub;
  return base / sub;
}

static bool fsync_path(const fs::path& path, std::string& err) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = "cannot open " + path.string() + " for fsync: " + std::strerror(errno);
    return false;
  }
  int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) {
    err = "fsync " + path.string() + " failed: " + std::strerror(errno);
    return false;
  }
  return true;
}

bool write_file_atomic(const fs::path& path, std::string_view content, std::string& err) {
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      err = "cannot open " + tmp.string() + " for writing";
      return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out) {
      err = "write to " + tmp.string() + " failed";
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    err = "rename " + tmp.string() + " -> " + path.string() + " failed: " + ec.message();
    return false;
  }
  // Durability: sync file contents and the directory entry.
  std::string sync_err;
  fsync_path(path, sync_err);  // best effort; ignore errors here
  fsync_path(path.parent_path().empty() ? fs::path(".") : path.parent_path(), sync_err);
  return true;
}

bool read_file(const fs::path& path, std::string& out, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot open " + path.string();
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) {
    err = "cannot determine size of " + path.string();
    return false;
  }
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  in.read(out.data(), size);
  if (!in && !in.eof()) {
    err = "read from " + path.string() + " failed";
    return false;
  }
  return true;
}

uint64_t file_size(const fs::path& path) {
  std::error_code ec;
  const auto s = fs::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(s);
}

bool free_space(const fs::path& path, uint64_t& free_bytes, uint64_t& total_bytes) {
  struct statvfs st{};
  if (::statvfs(path.c_str(), &st) != 0) return false;
  free_bytes = static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
  total_bytes = static_cast<uint64_t>(st.f_blocks) * static_cast<uint64_t>(st.f_frsize);
  return true;
}

std::string random_hex(size_t bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::random_device rd;
  std::string out;
  out.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t b = static_cast<uint8_t>(rd() & 0xFFu);
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0Fu]);
  }
  return out;
}

std::string hostname() {
  char buf[256];
  if (::gethostname(buf, sizeof(buf)) != 0) return "unknown";
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

std::string utc_timestamp_compact() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

std::string iso8601_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<long long>(ms));
  return buf;
}

}  // namespace protorecrep
