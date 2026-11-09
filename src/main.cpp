#include <cstdio>

#include "prp/app/record_app.h"
#include "prp/cli/options.h"
#include "prp/replay/replay_app.h"
#include "prp/util/log.h"

int main(int argc, char** argv) {
  prp::Options options;
  std::string err;
  if (!prp::parse_options(argc, argv, options, err)) {
    std::fprintf(stderr, "error: %s\n\n", err.c_str());
    prp::print_help();
    return 2;
  }
  if (options.help) {
    prp::print_help();
    return 0;
  }
  if (options.version) {
    std::printf("protorecprep %s\n", PRP_VERSION);
    return 0;
  }

  prp::set_log_level(options.log_level);

  if (options.command == "record") {
    return prp::run_record(options, err);
  }
  if (options.command == "replay") {
    const int rc = prp::run_replay(options, err);
    if (rc != 0 && !err.empty()) std::fprintf(stderr, "error: %s\n", err.c_str());
    return rc;
  }
  if (options.command == "inspect") {
    const int rc = prp::run_inspect(options, err);
    if (rc == 2 && !err.empty()) std::fprintf(stderr, "error: %s\n", err.c_str());
    return rc;
  }

  std::fprintf(stderr, "error: no command\n");
  return 2;
}
