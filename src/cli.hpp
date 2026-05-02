// Command-line argument parsing.
//
// Two top-level modes are mutually exclusive:
//   * Quick search mode (-p)        — uses Cli.search_*
//   * Script mode (-s / -script)    — uses Cli.script_*
//
// When neither is given, the first positional arg (if any) is treated as
// a script file path; otherwise the script is read from stdin (if piped).
#pragma once

#include "output.hpp"

#include <string>
#include <vector>

namespace hpr {

struct CliPattern {
    std::string regexp;
    bool case_insensitive = false; // set by -pi instead of -p
    // Comma-separated capture group names (set by `-extract` on the most
    // recently declared `-p`/`-pi`). Maps capture group i+1 → names[i].
    std::vector<std::string> extract_names;
};

struct Cli {
    // Quick-search options (-p mode).
    std::vector<CliPattern> patterns;
    std::vector<std::string> globs;
    std::vector<std::string> excludes;
    std::vector<std::string> positional;
    bool word_boundary = false;
    bool no_utf8 = false;        // -no-utf8: byte-mode matching
    bool ucp = false;            // -ucp: enable Unicode \w/\d/\s (opt-in)
    int64_t limit = -1;          // global match cap (-limit)
    int64_t per_file_limit = -1; // per-file cap (-m)

    OutputMode out_mode = OutputMode::JsonLines;
    bool out_mode_set = false;  // an explicit -j/-f/-c/-o/-format/-absent was given
    std::string format_template;
    int context_before = 0;
    int context_after = 0;

    // Block extraction (-block-open / -block-close). When both set, each
    // match's balanced block is extracted starting at match-end. With -o the
    // output becomes the full block (signature + body); the default JSONL
    // output gains `block`/`block_full`/`block_start`/`block_end`/
    // `block_line_start`/`block_line_end` fields; -format can reference
    // $BLOCK / $BLOCK_FULL / $BLOCK_START / $BLOCK_END / $BLOCK_LINE_START /
    // $BLOCK_LINE_END.
    std::string block_open;
    std::string block_close;

    // Byte budgets (0 = unlimited). Apply to text fields in every output mode
    // that emits per-match payloads. Truncation never errors; truncated fields
    // are flagged in JSONL output (`match_truncated`/`context_truncated`/
    // `block_truncated` and a top-level `truncated`). When `max_output_bytes`
    // is hit, scanning stops and a final `output_truncated` info record is
    // emitted from on_complete().
    uint64_t max_match_bytes = 0;
    uint64_t max_context_bytes = 0;
    uint64_t max_block_bytes = 0;
    uint64_t max_output_bytes = 0;

    // Enclosing-scope detection (`-scope=auto|go|rust|...` for built-in
    // packs, or generic `-scope-pattern`/`-scope-open`/`-scope-close`/
    // `-scope-kind` for custom). When set, every JSON record gains an
    // `enclosing` field and `-format` recognises `$ENCLOSING_*` tokens.
    std::string scope_lang;
    std::string scope_pattern;
    std::string scope_open;
    std::string scope_close;
    std::string scope_kind = "func";

    // Pattern relations (`-near` / `-far`). Filters matches of pattern `a` by
    // proximity to matches of pattern `b` within `lines` lines (0 = same
    // line). Multiple relations AND together. Both `a` and `b` are pattern
    // identifiers (`p0`, `p1`, …) or zero-based indices ("0", "1", …),
    // resolved against the final pattern list at runtime.
    enum class RelationKind { Near, Far };
    struct Relation {
        RelationKind kind = RelationKind::Near;
        std::string a;
        std::string b;
        int lines = 0;
    };
    std::vector<Relation> relations;

    // Sample mode (`-sample N`): collect matches across all files, then
    // emit ≤N representatives stratified by file and by a canonicalised
    // surrounding-line "shape" (identifiers replaced with `_`, whitespace
    // collapsed). 0 means streaming (default).
    int sample_n = 0;

    // Script mode.
    std::string script_inline;   // -s '<json>'
    std::string script_path;     // -script <path>

    // Misc.
    bool show_version = false;
    bool show_help = false;
    bool error = false;
    std::string error_message;
};

// Parse argv into Cli. Never exits — error/help are reported via flags.
Cli parse_cli(int argc, char **argv);

void print_help(FILE *out);

} // namespace hpr
