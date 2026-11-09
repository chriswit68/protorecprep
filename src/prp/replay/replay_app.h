#pragma once

#include <string>

#include "prp/cli/options.h"

namespace prp {

// Replays a capture file or a session directory of capture chunks to a remote
// TCP server. Returns 0 on success (including user interruption), 1 on a
// runtime failure and 2 on configuration/validation errors.
int run_replay(const Options& options, std::string& err);

}  // namespace prp
