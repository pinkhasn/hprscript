// Glue layer that ties walker + matcher + formatter for -p (search) mode.
#pragma once

#include "cli.hpp"

namespace hpr {

// Run quick-search mode (-p). Returns process exit code: 0 on at least one
// match, 1 on no matches, 2 on error (matches grep's convention).
int run_search(const Cli &cli);

} // namespace hpr
