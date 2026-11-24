#include "prp/json/blob.h"

#include "prp/lib.h"

namespace prp {
namespace {

bool has_magic(std::string_view data, std::string_view magic) {
  return data.size() >= magic.size() && data.substr(0, magic.size()) == magic;
}

bool is_utf8_text(std::string_view data) {
  size_t i = 0;
  while (i < data.size()) {
    const unsigned char c = static_cast<unsigned char>(data[i]);
    if (c < 0x80) {
      if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F) return false;
      ++i;
      continue;
    }
    size_t length = 0;
    uint32_t codepoint = 0;
    if ((c & 0xE0) == 0xC0) {
      length = 2;
      codepoint = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      length = 3;
      codepoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      length = 4;
      codepoint = c & 0x07;
    } else {
      return false;
    }
    if (i + length > data.size()) return false;
    for (size_t k = 1; k < length; ++k) {
      const unsigned char continuation = static_cast<unsigned char>(data[i + k]);
      if ((continuation & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }
    if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
        (length == 4 && codepoint < 0x10000) || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
    i += length;
  }
  return true;
}

std::string detect_kind(std::string_view data) {
  if (has_magic(data, std::string_view("\xd4\xc3\xb2\xa1", 4)) ||
      has_magic(data, std::string_view("\xa1\xb2\xc3\xd4", 4))) {
    return "pcap";
  }
  if (has_magic(data, std::string_view("\x0a\x0d\x0d\x0a", 4))) return "pcapng";
  if (data.size() >= 12 && has_magic(data, "RIFF") && data.substr(8, 4) == "WAVE") return "wav";
  if (has_magic(data, std::string_view("\x89PNG\r\n\x1a\n", 8))) return "png";
  if (has_magic(data, std::string_view("\xff\xd8\xff", 3))) return "jpeg";
  if (has_magic(data, "GIF87a") || has_magic(data, "GIF89a")) return "gif";
  if (has_magic(data, "%PDF-")) return "pdf";
  if (has_magic(data, std::string_view("PK\x03\x04", 4))) return "zip";
  if (has_magic(data, std::string_view("\x1f\x8b", 2))) return "gzip";
  if (has_magic(data, std::string_view("\x28\xb5\x2f\xfd", 4))) return "zstd";
  if (!data.empty() && is_utf8_text(data)) return "text";
  return "unknown";
}

std::string extension_for(const std::string& kind) {
  if (kind == "pcap") return "pcap";
  if (kind == "pcapng") return "pcapng";
  if (kind == "wav") return "wav";
  if (kind == "png") return "png";
  if (kind == "jpeg") return "jpg";
  if (kind == "gif") return "gif";
  if (kind == "pdf") return "pdf";
  if (kind == "zip") return "zip";
  if (kind == "gzip") return "gz";
  if (kind == "zstd") return "zst";
  if (kind == "text") return "txt";
  return "bin";
}

}  // namespace

BlobInfo analyze_blob(std::string_view payload) {
  BlobInfo info;
  info.data = payload;
  // Blob channels prepend a u32 LE flag/counter (observed value 1) in front of
  // the complete file. Strip it when present so the extracted file is intact.
  if (payload.size() >= 4 && get_u32le(reinterpret_cast<const uint8_t*>(payload.data())) == 1) {
    info.has_prefix = true;
    info.prefix = 1;
    info.data.remove_prefix(4);
  }
  info.kind = detect_kind(info.data);
  info.extension = extension_for(info.kind);
  return info;
}

}  // namespace prp
