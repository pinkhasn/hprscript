// Output formatting for -p mode.
//
// One Formatter sees every match in scan order. The formatter buffers
// per-file state for modes that need it (-f dedupes file names; -c counts
// per file). Match-streaming modes (default JSONL, -o) print as soon
// as a match arrives.
#pragma once

#include "common.hpp"
#include "extract.hpp"
#include "line_index.hpp"
#include "scope.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hpr {

class SeenStore;    // src/seen.hpp — -seen cross-invocation dedup
struct SeenMark;

enum class OutputMode {
    JsonLines,    // one JSON object per line (default)
    FilesOnly,    // grep -l: dedup'd file paths
    Counts,       // grep -c: file\tcount
    MatchOnly,    // grep -o: matched text only
    Custom,       // -format template with $FILE etc.
    Absent,       // grep -L: files where pattern is NOT present
    Llm,          // -llm: token-efficient text for LLM consumption
    Elide,        // -elide: scope-aware chunk rendering, whole file at once
};

// True iff the given output mode reads line/col/context from a LineIndex.
// Other modes can skip building the index entirely.
inline bool needs_line_index(OutputMode m) {
    switch (m) {
        case OutputMode::JsonLines:
        case OutputMode::Custom:
        case OutputMode::Llm:
        case OutputMode::Elide:
            return true;
        case OutputMode::FilesOnly:
        case OutputMode::Counts:
        case OutputMode::MatchOnly:
        case OutputMode::Absent:
            return false;
    }
    return true;
}

struct OutputOptions {
    OutputMode mode = OutputMode::JsonLines;
    std::string format_template;
    int context_before = 0;
    int context_after = 0;

    // When both are set, each match is paired with its balanced block
    // (searched from match-start). Affects every output mode that emits per-match
    // records: -o prints `$BLOCK_FULL` instead of just `$MATCH`; the default
    // JSONL output gains `block` / `block_full` / `block_start` / `block_end`
    // / `block_line_start` / `block_line_end` fields; -format gains the
    // matching `$BLOCK*` tokens.
    std::string block_open;
    std::string block_close;

    // Byte budgets (0 = unlimited). See Cli for semantics.
    uint64_t max_match_bytes = 0;
    uint64_t max_context_bytes = 0;
    uint64_t max_block_bytes = 0;
    uint64_t max_output_bytes = 0;

    // When non-null, capture groups for matched patterns are re-extracted
    // and surfaced in JSON / format-template output. Pointer is borrowed.
    const ExtractTable *extract_table = nullptr;

    // LLM mode only: total pattern count (so the formatter can decide whether
    // to prefix records with the pattern id) and the effective global limit
    // (so on_complete can emit a "limit reached" footer).
    size_t pattern_count = 1;
    int64_t global_limit = -1;
};

class Formatter {
public:
    Formatter(OutputOptions opts, FILE *out);

    void on_match(const std::string &file, const Pattern &pattern,
                  const Match &m, std::string_view buf, const LineIndex &idx,
                  const ScopeIndex *scope = nullptr);

    // -elide mode: render one file's entire kept-match set in a single call,
    // grouped by innermost enclosing scope. Each scope prints its signature
    // line, then matched lines with -A/-B context, folding untouched
    // interior lines as "… (+N lines)"; matches outside any scope fall back
    // to a plain context-block line (like -llm's line branch). `kept` must
    // be sorted by `from` ascending (the collector's normal emission order).
    //
    // `seen` (optional, -seen): before rendering a scope block in full, its
    // raw byte range is hashed and checked against `seen` — an unchanged
    // match collapses to a one-line "(unchanged, already shown)" pointer
    // instead of the full render. Orphan (no-scope) matches are never
    // collapsed. `marks_out` (optional), when given, receives one SeenMark
    // per scope block examined (collapsed or not) — the caller decides
    // whether to actually commit those into the persistent SeenStore (see
    // src/seen.hpp's SeenMark doc comment for why that's a separate step).
    void on_file_elide(const std::string &file, const std::vector<Match> &kept,
                       std::string_view buf, const LineIndex &idx,
                       const ScopeIndex *scope, const SeenStore *seen = nullptr,
                       std::vector<SeenMark> *marks_out = nullptr);

    // Record-level absence (-records line with -absent): emit one JSON line
    // for a record (line) of `file` that lacks `pattern`. Only meaningful in
    // Absent mode; `text` is the record's content (truncated to
    // max_context_bytes).
    void on_record_absent(const std::string &file, const Pattern &pattern,
                          uint32_t line, std::string_view text);

    // Called after a file's scan finishes. Required for FilesOnly when
    // there were no matches (-absent), and lets Counts emit one row.
    void on_file_end(const std::string &file, bool had_match);

    // Called once after all files are processed. Flushes any deferred
    // aggregate output and emits an `output_truncated` info record when the
    // byte cap was hit during scanning.
    void on_complete();

    uint64_t emitted() const { return emitted_; }

    // True after a per-match emission first pushes total stdout bytes past
    // `max_output_bytes`. Once set, the runner should stop walking files.
    bool over_budget() const { return over_budget_; }

    // Runner calls this when it stops emitting because the global limit was
    // hit. LLM mode prints a "limit reached" footer in on_complete().
    void mark_limit_hit() { limit_hit_ = true; }

private:
    void emit_json(const std::string &file, const Pattern &pattern,
                   const Match &m, std::string_view buf, const LineIndex &idx,
                   const ScopeIndex *scope);
    void emit_match_only(std::string_view buf, const Match &m,
                         const LineIndex &idx);
    void emit_custom(const std::string &file, const Pattern &pattern,
                     const Match &m, std::string_view buf, const LineIndex &idx,
                     const ScopeIndex *scope);
    void emit_llm(const std::string &file, const Pattern &pattern,
                  const Match &m, std::string_view buf, const LineIndex &idx,
                  const ScopeIndex *scope);

    // Compute the joined context block (prev N lines + match line + next N
    // lines) as a single string view into buf. Trailing newline is excluded.
    std::string_view context_block(std::string_view buf, const LineIndex &idx,
                                   const Match &m) const;

    // Cache the JSON-escaped file path so repeat matches on the same file
    // don't re-escape. Compared by string value, not pointer.
    void refresh_file_cache(const std::string &file);
    void refresh_pattern_cache(const Pattern &pattern);

    // Single write funnel — updates bytes_emitted_ and over_budget_.
    void write_out(const char *data, size_t n);
    void write_out(std::string_view sv) { write_out(sv.data(), sv.size()); }

    OutputOptions opts_;
    FILE *out_;
    uint64_t emitted_ = 0;
    uint64_t bytes_emitted_ = 0; // total bytes written to out_
    bool over_budget_ = false;   // sticky once max_output_bytes is exceeded
    bool limit_hit_ = false;     // set by mark_limit_hit() (LLM footer)
    std::string llm_last_file_;  // last file printed as a header (-llm/-elide dedupe)
    std::unordered_map<std::string, uint64_t> per_file_counts_;

    // Per-line scratch buffer reused across matches to avoid reallocation.
    std::string scratch_;

    // Cached JSON-escaped representations of the current file/pattern so
    // we don't re-escape unchanged values for every match.
    std::string cached_file_;
    std::string cached_file_esc_;
    const Pattern *cached_pat_ = nullptr;
    std::string cached_pat_id_esc_;
};

// JSON string escape (\, ", control chars, multibyte left untouched).
std::string json_escape(std::string_view s);
// Append the JSON-escaped form of s to out (no surrounding quotes).
void json_escape_to(std::string &out, std::string_view s);

// Scan accounting shared by -p and script mode, driving -summary /
// -diagnostics / -require-complete.
struct ScanStats {
    uint64_t files_scanned = 0;  // files whose content was actually scanned
    uint64_t bytes_scanned = 0;  // exact input bytes presented to matchers
    uint64_t files_binary = 0;   // skipped: NUL byte in the first 512 bytes
    uint64_t files_failed = 0;   // open/mmap failures
    uint64_t missing_paths = 0;  // -files-from entries that didn't exist
    uint64_t matches_seen = 0;   // matches surviving dedup + relations
    uint64_t scan_stages = 0;    // repository/stdin traversal stages executed
    uint64_t matcher_compilations = 0;
    uint64_t patterns_compiled = 0;
    uint64_t rows_materialized = 0;
    uint64_t rows_output = 0;
    uint64_t rows_truncated = 0;
    uint64_t buffered_bytes_peak = 0;
    std::string stop_reason;     // "", "limit", "output_budget"

    bool complete() const {
        return stop_reason.empty() && files_failed == 0 && missing_paths == 0;
    }
};

// One {"type":"warning","code":...,"file":...} JSON line on stdout
// (-diagnostics mode).
void emit_warning_record(const char *code, const std::string &path);

// The trailing {"type":"summary",...} record (-summary mode). `emitted` is
// the number of output records actually written; stop_reason is emitted
// only when non-empty.
void emit_summary_record(const ScanStats &st, uint64_t emitted,
                         uint64_t elapsed_ms);

} // namespace hpr
