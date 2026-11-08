#pragma once

#include <string>

#include "prp/cli/options.h"

namespace prp {

int run_record(const Options& options, std::string& err);
int run_inspect(const Options& options, std::string& err);

}  // namespace prp
