#pragma once

#include <filesystem>

#include "protorecrep/protorecrep.h"

// The application keeps its historical `prp` namespace. These aliases expose
// the standalone capture core under the names the existing code already uses.
namespace prp {

namespace fs = std::filesystem;

namespace rec = protorecrep::rec;

using protorecrep::BoundedItemQueue;
using protorecrep::BoundedQueue;
using protorecrep::CaptureItem;
using protorecrep::CaptureReader;
using protorecrep::CaptureRecord;
using protorecrep::CloseReason;
using protorecrep::Direction;
using protorecrep::ILogger;
using protorecrep::ItemKind;
using protorecrep::ItemPtr;
using protorecrep::OverflowPolicy;
using protorecrep::PushResult;
using protorecrep::QueueStats;
using protorecrep::ReadStatus;
using protorecrep::Recorder;
using protorecrep::RecorderConfig;
using protorecrep::RecorderStats;
using protorecrep::ScanStats;

using protorecrep::close_reason_name;
using protorecrep::list_capture_files;
using protorecrep::make_data;
using protorecrep::make_marker;
using protorecrep::make_stream_close;
using protorecrep::make_stream_open;
using protorecrep::read_capture_header;
using protorecrep::recover_session_dir;
using protorecrep::scan_capture_file;

using protorecrep::Sha256;
using protorecrep::Sha256Digest;
using protorecrep::crc32c;
using protorecrep::crc32c_extend;

using protorecrep::ensure_directory;
using protorecrep::file_size;
using protorecrep::free_space;
using protorecrep::hostname;
using protorecrep::iso8601_now;
using protorecrep::random_hex;
using protorecrep::read_file;
using protorecrep::resolve_subdir;
using protorecrep::utc_timestamp_compact;
using protorecrep::write_file_atomic;

using protorecrep::format_bytes;
using protorecrep::json_escape;
using protorecrep::now_monotonic_ns;
using protorecrep::now_wall_ns;

using protorecrep::append_bytes;
using protorecrep::append_u16le;
using protorecrep::append_u32le;
using protorecrep::append_u64le;
using protorecrep::get_u16le;
using protorecrep::get_u32le;
using protorecrep::get_u64le;
using protorecrep::load_u64le_unaligned;
using protorecrep::put_u16le;
using protorecrep::put_u32le;
using protorecrep::put_u64le;

}  // namespace prp
