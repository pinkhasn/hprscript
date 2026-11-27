// Glue layer that ties walker + matcher + formatter for -p (search) mode.
#pragma once

#include "cli.hpp"

namespace hpr {

// Run quick-search mode (-p). Returns process exit code: 0 on at least one
// match, 1 on no matches, 2 on error (matches grep's convention).
int run_search(const Cli &cli);

// Run -list-scopes mode: dump the scope index (one record per detected
// function/class) with no patterns involved. Honors -in-scope /
// -in-scope-kind as a filter and -limit as a cap; -scope defaults to auto.
// Exit codes follow grep: 0 = scopes listed, 1 = none found, 2 = error.
int run_list_scopes(const Cli &cli);

// Run -list-regions mode: dump the pattern-defined region index (one
// record per region, in file order) with no search patterns involved.
// Honors -in-region/-not-in-region as a filter and -limit as a cap.
// Exit codes follow grep: 0 = regions listed, 1 = none found, 2 = error.
int run_list_regions(const Cli &cli);

} // namespace hpr
