// Script mode (-s / -script) — runs a JSON program against scanned files.
//
// Supports an MVP subset of SRSCRIPT.md: top-level "scan", "exclude",
// "patterns", "context"/"context_before"/"context_after", "limit",
// "limit_per_file"; per-pattern "id", "regexp", "case_insensitive",
// "word_boundary"; on_match action "emit" (with optional "data" mapping
// containing variable substitutions like $FILE, $LINE, $MATCH).
//
// Phases, variables, submatch, blocks, ranking, grouping, and file
// modification are out of scope for the MVP and reported as errors.
#pragma once

#include "cli.hpp"

namespace hpr {

int run_script(const Cli &cli);

} // namespace hpr
