#include "prp/proto/schema.h"

#include <set>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/json/json.h>

#include "prp/lib.h"
#include "prp/util/log.h"

namespace prp {
namespace {

class ErrorCollector : public google::protobuf::compiler::MultiFileErrorCollector {
 public:
  void RecordError(absl::string_view filename, int line, int column, absl::string_view message) override {
    std::string text;
    if (!filename.empty()) text += std::string(filename) + ":";
    if (line >= 0) text += std::to_string(line + 1) + ":" + std::to_string(column + 1) + ": ";
    text += std::string(message);
    if (!messages_.empty()) messages_ += "; ";
    messages_ += text;
  }

  const std::string& messages() const { return messages_; }

 private:
  std::string messages_;
};

std::string strip_leading_dot(const std::string& name) {
  return (!name.empty() && name.front() == '.') ? name.substr(1) : name;
}

}  // namespace

struct Schema::Impl {
  std::unique_ptr<google::protobuf::compiler::DiskSourceTree> source_tree;
  std::unique_ptr<ErrorCollector> errors;
  std::unique_ptr<google::protobuf::compiler::Importer> importer;
  const google::protobuf::DescriptorPool* pool = nullptr;
  const google::protobuf::FileDescriptor* main = nullptr;
  std::string virtual_name;
  Sha256Digest digest{};
  std::string digest_hex;
};

Schema::Schema() : impl_(std::make_unique<Impl>()) {}
Schema::~Schema() = default;

bool Schema::load(const fs::path& proto_file, const std::vector<fs::path>& include_paths, std::string& err) {
  std::error_code ec;
  if (!fs::exists(proto_file, ec)) {
    err = "proto file not found: " + proto_file.string();
    return false;
  }

  std::string content;
  if (!read_file(proto_file, content, err)) return false;
  impl_->digest = Sha256::digest(content);
  impl_->digest_hex = Sha256::hex(impl_->digest);

  impl_->source_tree = std::make_unique<google::protobuf::compiler::DiskSourceTree>();
  // Search order: explicit include paths first, then the proto file's own
  // directory. The proto's parent maps to the virtual root "" so that
  // imports resolve the same way protoc would.
  for (const auto& path : include_paths) {
    std::error_code iec;
    if (!fs::is_directory(path, iec)) {
      log_printf(LogLevel::Warn, "proto include path does not exist: %s", path.string().c_str());
      continue;
    }
    impl_->source_tree->MapPath("", path.string());
  }
  const fs::path parent = proto_file.parent_path();
  impl_->source_tree->MapPath("", parent.empty() ? std::string(".") : parent.string());

  impl_->errors = std::make_unique<ErrorCollector>();
  impl_->importer =
      std::make_unique<google::protobuf::compiler::Importer>(impl_->source_tree.get(), impl_->errors.get());

  impl_->virtual_name = proto_file.filename().string();
  impl_->main = impl_->importer->Import(impl_->virtual_name);
  if (impl_->main == nullptr) {
    err = "failed to load " + proto_file.string();
    if (!impl_->errors->messages().empty()) err += ": " + impl_->errors->messages();
    return false;
  }
  impl_->pool = impl_->importer->pool();
  return true;
}

bool Schema::loaded() const { return impl_->main != nullptr; }

const google::protobuf::Descriptor* Schema::find_message(const std::string& name) const {
  if (!impl_->pool) return nullptr;
  const std::string normalized = strip_leading_dot(name);
  const google::protobuf::Descriptor* desc = impl_->pool->FindMessageTypeByName(normalized);
  if (!desc) {
    // Allow unqualified names if they are unambiguous.
    desc = impl_->pool->FindMessageTypeByName(name);
  }
  return desc;
}

const google::protobuf::DescriptorPool* Schema::pool() const { return impl_->pool; }
const google::protobuf::FileDescriptor* Schema::main_file() const { return impl_->main; }

bool Schema::build_descriptor_set(std::string& out, std::string& err) const {
  if (!impl_->main) {
    err = "schema not loaded";
    return false;
  }
  google::protobuf::FileDescriptorSet set;
  std::set<std::string> seen;
  std::vector<const google::protobuf::FileDescriptor*> stack{impl_->main};
  while (!stack.empty()) {
    const google::protobuf::FileDescriptor* fd = stack.back();
    stack.pop_back();
    if (!seen.insert(std::string(fd->name())).second) continue;
    for (int i = 0; i < fd->dependency_count(); ++i) stack.push_back(fd->dependency(i));
    google::protobuf::FileDescriptorProto* proto = set.add_file();
    fd->CopyTo(proto);
  }
  if (!set.SerializeToString(&out)) {
    err = "could not serialize descriptor set";
    return false;
  }
  return true;
}

const Sha256Digest& Schema::digest() const { return impl_->digest; }
std::string Schema::digest_hex() const { return impl_->digest_hex; }

bool message_to_json(const google::protobuf::Message& message, bool print_defaults, bool preserve_names,
                     std::string& out, std::string& err) {
  google::protobuf::json::PrintOptions options;
  options.add_whitespace = false;
  options.always_print_fields_with_no_presence = print_defaults;
  options.preserve_proto_field_names = preserve_names;
  const absl::Status status = google::protobuf::json::MessageToJsonString(message, &out, options);
  if (!status.ok()) {
    err = std::string(status.message());
    return false;
  }
  return true;
}

bool json_to_message(const std::string& json, google::protobuf::Message& message, bool ignore_unknown,
                     std::string& err) {
  google::protobuf::json::ParseOptions options;
  options.ignore_unknown_fields = ignore_unknown;
  const absl::Status status = google::protobuf::json::JsonStringToMessage(json, &message, options);
  if (!status.ok()) {
    err = std::string(status.message());
    return false;
  }
  return true;
}

}  // namespace prp
