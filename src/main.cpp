// hprscript — multi-pattern PCRE search powered by Vectorscan.
//
// Entry point. Parses the CLI and dispatches to one of:
//   - run_edit():   `edit` subcommand (the only mode that writes files)
//   - run_search(): -p quick-search mode
//   - run_script(): -s/-script JSON-script mode
//
// Both modes share the walker, matcher, and formatter underneath; the
// difference is whether patterns/options are supplied via flags or JSON.

#include "cli.hpp"
#include "edit.hpp"
#include "runner.hpp"
#include "script.hpp"

#include <cstdio>
#include <hs/hs.h>

// Version is injected at build time from `git describe` (see Makefile).
// The fallback keeps a direct compile of this file working without the -D.
#ifndef HPRSCRIPT_VERSION
#define HPRSCRIPT_VERSION "v0.2.1"
#endif

int main(int argc, char **argv) {
    hpr::Cli cli = hpr::parse_cli(argc, argv);

    if (!cli.error && !cli.patterns_from.empty()) {
        if (!cli.script_inline.empty() || !cli.script_path.empty()) {
            cli.error = true;
            cli.error_message = "-patterns-from cannot be combined with "
                                "-s/-script (use the script's patterns array)";
        } else {
            hpr::load_patterns_from(cli);
        }
    }
    if (!cli.error) hpr::validate_pattern_ids(cli);

    if (cli.error) {
        std::fprintf(stderr, "hprscript: %s\n", cli.error_message.c_str());
        std::fprintf(stderr, "  run 'hprscript --help' for usage\n");
        return 2;
    }
    if (cli.show_help) { hpr::print_help(stdout); return 0; }
    if (cli.show_version) {
        std::printf("hprscript %s (vectorscan %s)\n", HPRSCRIPT_VERSION, hs_version());
        return 0;
    }

    // Edit subcommand (`hprscript edit …`) — the only mode that can write.
    if (cli.edit.active) {
        return hpr::run_edit(cli);
    }

    // -list-scopes: dump the scope index, no patterns involved.
    if (cli.list_scopes) {
        return hpr::run_list_scopes(cli);
    }

    // Quick-search (-p) mode is selected when at least one -p was given.
    if (!cli.patterns.empty()) {
        return hpr::run_search(cli);
    }

    // Otherwise we're in script mode: -s, -script, or implicit (positional
    // arg as script file / stdin pipe).
    return hpr::run_script(cli);
}
