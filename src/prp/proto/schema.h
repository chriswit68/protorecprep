#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include "prp/lib.h"

namespace prp {

namespace fs = std::filesystem;

// Loads a .proto file at runtime via libprotoc's Importer and exposes its
// descriptor pool. The schema is immutable after load() and can be read from
// multiple threads (every thread uses its own DynamicMessageFactory).
class Schema {
 public:
  Schema();
  ~Schema();

  Schema(const Schema&) = delete;
  Schema& operator=(const Schema&) = delete;

  bool load(const fs::path& proto_file, const std::vector<fs::path>& include_paths, std::string& err);

  bool loaded() const;
  const google::protobuf::Descriptor* find_message(const std::string& name) const;
  const google::protobuf::DescriptorPool* pool() const;
  const google::protobuf::FileDescriptor* main_file() const;

  // Serialized google.protobuf.FileDescriptorSet (main file + dependencies).
  bool build_descriptor_set(std::string& out, std::string& err) const;

  const Sha256Digest& digest() const;
  std::string digest_hex() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool message_to_json(const google::protobuf::Message& message, bool print_defaults, bool preserve_names,
                     std::string& out, std::string& err);
bool json_to_message(const std::string& json, google::protobuf::Message& message, bool ignore_unknown,
                     std::string& err);

}  // namespace prp
