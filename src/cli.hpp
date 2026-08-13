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
    bool fixed = false;            // declared through -F/-Fi/literal
    // Pattern id override (set by `-name` on the most recently declared
    // `-p`/`-pi`, or by `id` in a -patterns-from file). Empty → auto `p<i>`.
    // Shown as `pat`/$PAT_ID and usable as the A/B side of relations.
    std::string name;
    // Free-text meaning (set by `-desc` on the most recently declared
    // pattern, or by `description` in a -patterns-from file). When any
    // pattern has one, -llm/-elide output opens with a query-legend header.
    std::string desc;
    // Comma-separated capture group names (set by `-extract` on the most
    // recently declared `-p`/`-pi`). Maps capture group i+1 → names[i].
    std::vector<std::string> extract_names;
    // Tri-state per-pattern overrides used by -patterns-from entries:
    // -1 = inherit the global flag (-w / -no-utf8), 0/1 = explicit.
    int word_boundary = -1;
    int utf8 = -1;
    // Edit mode only (-ref): reference-only pattern — usable in relations
    // and -file-where, but its matches never become edit sites.
    bool ref = false;

    // -ident terms (already lowercased at parse time). Non-empty marks this
    // entry as an identifier-subtoken group instead of a regex pattern —
    // `regexp` is unused for it. Terms inside one group AND together;
    // separate -ident occurrences OR together (each is its own CliPattern
    // entry, auto-numbered ident0/ident1/... independently of regex
    // patterns' p0/p1/... numbering). See src/ident.hpp.
    std::vector<std::string> ident_terms;
};

// Options for the `edit` subcommand — the only mode allowed to modify
// files. Search (-p) and script (-s) modes stay strictly read-only; edit is
// a distinct argv[1] verb so command-prefix permission systems can gate it
// separately. Dry-run is the default; -write applies. See the "Edit mode"
// chapter in HPRSCRIPT.md.
struct EditOptions {
    bool active = false; // argv[1] was "edit"

    // What byte range each match resolves to. Block spans reuse the
    // documented $BLOCK/$BLOCK_FULL semantics; scope spans use the
    // enclosing ScopeRange.
    enum class Span { Match, Line, Block, BlockFull, Scope, ScopeBody };
    Span span = Span::Match;

    enum class Verb { Replace, Insert, Delete };
    Verb verb = Verb::Replace;

    // -insert position relative to the resolved span. Before/After = outer
    // edges (any span); Start/End = just inside the delimiters (block /
    // scope-body only).
    enum class InsertPos { Before, After, Start, End };
    InsertPos insert_pos = InsertPos::Before;

    // Content source — exactly one for replace/insert, none for -delete.
    bool content_set = false;   // -content given (may be empty string)
    std::string content;        // inline template ($MATCH, $EXTRACT_*, …)
    std::string content_file;   // -content-file path
    bool content_stdin = false; // -content-stdin

    bool write = false; // -write: apply; default is dry-run preview
    bool diff = false;  // -diff: with -write, also print the unified diff
    std::string plan_out;          // -plan-out: persistent immutable preview
    std::string plan_format = "json";
    bool follow_symlinks = false;  // must also be repeated during apply
    bool no_plan_warning = false;  // compatibility escape hatch for -write

    // Guards — all checked before anything is written; violations exit 3.
    int64_t expect = -1;          // exact edit-site count, -1 = off
    int64_t max_span_lines = 500; // refuse larger spans; 0 = unlimited
    std::string assert_contains;  // every span must match this regex
};

struct ApplyOptions {
    bool active = false; // argv[1] was "apply"
    std::string plan_path;
    bool diff = false;
    bool json = true;
    bool follow_symlinks = false;
    bool allow_root_move = false;
    std::string receipt = "json";
    // Internal compatibility path only: direct edit -write applies the plan
    // created in the same process and keeps explicit absolute input paths.
    // No CLI flag sets this.
    bool trusted_in_memory = false;
};

struct InvestigateOptions {
    bool active = false;
    std::string profile = "auto";
    int top_files = 8;
    int top_scopes = 12;
    int related = 20;
    int examples = 12;
    enum class Followup { Auto, Always, Never } followup = Followup::Auto;
    int max_related_patterns = 64;
    uint64_t evidence_budget = 65536;
    uint64_t max_memory_bytes = 128ull * 1024 * 1024;
};

struct QueryOptions {
    bool active = false;
    std::string inline_json;
    std::string path;
};

struct Cli {
    // Original argv, retained for immutable edit-plan provenance.
    std::vector<std::string> command;
    // Quick-search options (-p mode).
    std::vector<CliPattern> patterns;
    std::vector<std::string> globs;
    std::vector<std::string> excludes;
    std::vector<std::string> positional;

    // File-list inputs (-files-from / -files0-from): each names a list file
    // ("-" = stdin) of literal paths to scan — newline-delimited, or
    // NUL-delimited when `nul` is set. Entries are never glob-interpreted,
    // so filenames containing *, {, [ are safe. Works in both -p and script
    // mode (in script mode the list overrides the script's `scan`, like
    // positional paths do).
    struct FileList {
        std::string path;
        bool nul = false;
    };
    std::vector<FileList> file_lists;

    // Git-aware input selection (-git-changed / -git-staged / -git-untracked
    // / -git-range). Selected files join the literal-path pipeline exactly
    // like -files-from entries. -git-added-lines further restricts matches
    // to lines added by the selected diffs (quick mode only; requires a
    // diff-based selection flag).
    bool git_changed = false;
    bool git_staged = false;
    bool git_untracked = false;
    std::vector<std::string> git_ranges;
    bool git_added_lines = false;

    // Pattern-file inputs (-patterns-from): JSONL rule files, one pattern
    // object per line ({id, regexp | literal, case_insensitive,
    // word_boundary, utf8}); `#` lines and blank lines are ignored. Loaded
    // by load_patterns_from() after parsing; entries append to `patterns`.
    std::vector<std::string> patterns_from;

    // Per-file boolean predicate (-file-where): expression over pattern ids
    // (AND/OR/NOT, &&/||/!, parens). A file's matches are emitted only when
    // the predicate holds over the set of patterns that matched in it.
    std::string file_where;

    // -order-by <score|count|path>: sort -f/-c output instead of streaming
    // in walk order. `score` reuses the -hotspots ranking formula; `count`
    // is total matches in the file; `path` is lexicographic. Mutually
    // exclusive with -sample/-hotspots/-budget, which already define their
    // own ordering.
    enum class OrderBy { None, Score, Count, Path };
    OrderBy order_by = OrderBy::None;

    // -seen <path>: cross-invocation dedup state file for -elide/-budget.
    // Scope chunks unchanged since the last run against this file collapse
    // to a one-line pointer instead of full source. Empty = off (default).
    std::string seen_path;
    bool word_boundary = false;
    bool no_utf8 = false;        // -no-utf8: byte-mode matching
    // -no-roles: disable per-match role classification (def/comment/string/
    // import — the `role` JSONL field, -llm bracket tags, and $ROLE). On by
    // default for per-match output modes; roles are computed lazily per
    // matched file, so the cost only exists where output is produced.
    bool no_roles = false;
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
    // identifiers (names, `p0`, `p1`, …) or zero-based indices ("0", "1", …),
    // resolved against the final pattern list at runtime.
    //
    // SameScope / NotSameScope (`-same-scope` / `-not-same-scope`) test for a
    // `b` match inside the same innermost enclosing scope instead of a line
    // distance (`lines` is unused). They require an active -scope config;
    // `a` matches outside any recognized scope are dropped by SameScope and
    // kept by NotSameScope. When a == b, "same scope" needs a *second* match.
    enum class RelationKind { Near, Far, SameScope, NotSameScope };
    struct Relation {
        RelationKind kind = RelationKind::Near;
        std::string a;
        std::string b;
        int lines = 0;
    };
    std::vector<Relation> relations;

    // Record mode (-records). v1 supports `line`: combined with -absent,
    // emit one record per non-empty line lacking each pattern (record-level
    // absence) instead of the per-file file list.
    enum class RecordMode { None, Line };
    RecordMode records = RecordMode::None;

    // Scoped targeting (-in-scope / -in-scope-kind): keep only matches whose
    // enclosing-scope chain contains a scope whose name matches one of these
    // regexes (ECMAScript, search semantics; OR) and whose kind equals
    // in_scope_kind when set. Implies `-scope auto` when no scope config was
    // given. Works in search and edit mode; in edit mode it also selects the
    // targets of anchorless `-span scope|scope-body` edits.
    std::vector<std::string> in_scopes;
    std::string in_scope_kind;

    // Line-range restriction (-lines): keep only matches starting within one
    // of these 1-based inclusive ranges (OR). Forms: "N", "A:B", "A:", ":B".
    struct LineRange {
        uint32_t lo = 1;
        uint32_t hi = 0xffffffffu;
    };
    std::vector<LineRange> line_ranges;

    // -list-scopes: dump the scope index (name/kind/lines per scope) instead
    // of searching — no patterns involved. Honors -in-scope/-in-scope-kind
    // as a filter; output is JSONL (default) or -llm flat lines.
    bool list_scopes = false;

    // Sample mode (`-sample N`): collect matches across all files, then
    // emit ≤N representatives stratified by file and by a canonicalised
    // surrounding-line "shape" (identifiers replaced with `_`, whitespace
    // collapsed). 0 means streaming (default).
    int sample_n = 0;

    // Ranking mode (`-hotspots N`): buffer matches across the whole scan,
    // then emit the top N files by the same rarity/coverage/proximity
    // score script mode's `rank` uses (see src/rank.hpp), each annotated
    // with its densest match window. 0 means off (default).
    int hotspots_n = 0;

    // Budget-packing mode (`-budget N`): buffer the whole scan, rank every
    // matching file, then render score-descending in -elide's shape until
    // N bytes are spent — degrading to a one-line summary and finally to
    // "dropped" once a file's full render no longer fits. Defines its own
    // output shape, so it's mutually exclusive with -j/-f/-c/-o/-format/
    // -absent/-llm/-elide/-sample/-hotspots. 0 = off (default).
    uint64_t budget_bytes = 0;

    // Script mode.
    std::string script_inline;   // -s '<json>'
    std::string script_path;     // -script <path>

    // Scan accounting (work in both -p and script mode).
    // -summary: emit a trailing {"type":"summary",...} record with scan
    // accounting (files scanned/skipped/failed, matches, completeness).
    bool summary = false;
    // -diagnostics: emit {"type":"warning","code":...,"file":...} records on
    // stdout for read errors, binary skips, and missing file-list paths
    // (replaces the stderr text for those cases).
    bool diagnostics = false;
    // -require-complete: exit 2 when any file could not be read or a listed
    // path was missing — silent partial results become hard failures.
    bool require_complete = false;

    // Deterministic execution-plan reporting. -explain-plan emits the plan
    // as the first JSONL record; -plan-only implies it and returns before any
    // input traversal.
    bool explain_plan = false;
    bool plan_only = false;

    // Edit subcommand (`hprscript edit …`). See EditOptions.
    EditOptions edit;
    ApplyOptions apply;
    InvestigateOptions investigate;
    QueryOptions query;

    // Misc.
    bool show_version = false;
    bool show_help = false;
    bool error = false;
    std::string error_message;
};

// Parse argv into Cli. Never exits — error/help are reported via flags.
Cli parse_cli(int argc, char **argv);

// Load -patterns-from files into cli.patterns (JSONL entries; `#` comments).
// Returns false with cli.error set on the first malformed entry.
bool load_patterns_from(Cli &cli);

// Check final pattern ids (explicit names + auto p<i>) for duplicates.
// Returns false with cli.error set on collision.
bool validate_pattern_ids(Cli &cli);

void print_help(FILE *out);

} // namespace hpr
