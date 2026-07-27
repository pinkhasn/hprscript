// The `edit` subcommand — the only mode allowed to modify files.
//
// Reuses the search pipeline (pipeline.hpp) for targeting, then resolves
// each surviving match to an edit span, validates every guard, and either
// previews (unified diff, the default) or applies (-write) the splices.
// See the "Edit mode" chapter in HPRSCRIPT.md for the full contract.
#pragma once

#include "cli.hpp"

namespace hpr {

// Run edit mode. Exit codes: 0 = edits applied or previewed, 1 = no edit
// sites matched, 2 = usage/IO error, 3 = guard violation (nothing written).
int run_edit(const Cli &cli);

} // namespace hpr
