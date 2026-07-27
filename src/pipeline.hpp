// Shared per-file match production used by the search runner (-p mode) and
// the edit runner. Everything here is emission-agnostic: it turns (patterns,
// filters, one file buffer) into the final `kept` match list —
// raw Vectorscan hits → leftmost-longest dedup → emission-order sort →
// -git-added-lines filter → -near/-far/-same-scope relation filters — plus
// the setup-stage helpers both runners need: pattern building, relation
// resolution, the -file-where predicate, and per-file scope construction.
#pragma once

#include "cli.hpp"
#include "common.hpp"
#include "git.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hpr {

// True when the first 512 bytes contain a NUL — treated as binary and
// skipped by both runners.
bool looks_binary(std::string_view content);

// Add all CLI-selected inputs to the walker: -glob / positional / -exclude,
// -files-from / -files0-from lists, and git file selections; also loads the
// -git-added-lines map when active. Missing listed paths warn (or emit
// diagnostics records) and are counted in `stats`. Returns false on a hard
// error (the caller should exit 2).
bool add_walker_inputs(const Cli &cli, Walker &walker, ScanStats &stats,
                       std::unordered_map<std::string, AddedLines> &added);

// Build the final Pattern list from CLI state: auto `p<i>` ids, per-pattern
// name/word_boundary/utf8 overrides (-name / -patterns-from entries), global
// -w / -no-utf8 / -ucp defaults.
std::vector<Pattern> build_patterns(const Cli &cli);

// Resolve a pattern identifier — explicit name, auto "p<i>", or a bare
// zero-based index ("0", "1", …) — to its index in `patterns`.
bool resolve_pattern_id(const std::vector<Pattern> &patterns,
                        const std::string &id, uint32_t &out);

// A -near/-far/-same-scope relation with its a/b sides resolved to indices.
struct ResolvedRelation {
    Cli::RelationKind kind = Cli::RelationKind::Near;
    uint32_t a_idx = 0;
    uint32_t b_idx = 0;
    int lines = 0;
};

// Resolve CLI relations against the pattern list. On an unknown id, prints
// the usual stderr message and returns false.
bool resolve_relations(const std::vector<Cli::Relation> &relations,
                       const std::vector<Pattern> &patterns,
                       std::vector<ResolvedRelation> &out);

// True when any relation needs an active -scope config.
bool any_scope_relation(const std::vector<ResolvedRelation> &rels);

// -file-where: boolean predicate over pattern ids, evaluated per file
// against "did this pattern match at least once here".
class FileWhere {
public:
    // Expression tree node. Public so the parser (pipeline.cpp) can build
    // trees; not intended for use outside this module.
    struct Node {
        enum Kind { And, Or, Not, Leaf } kind = Leaf;
        std::string id;    // Leaf: pattern id as written
        uint32_t pat = 0;  // Leaf: resolved pattern index
        std::vector<Node> kids;
    };

    // Parse `expr` and resolve its leaves against `patterns`. An empty expr
    // leaves the predicate inactive and succeeds. On error, prints the usual
    // stderr message and returns false.
    bool init(const std::string &expr, const std::vector<Pattern> &patterns);

    bool active() const { return active_; }

    // Does the predicate hold for a file whose surviving matches are `kept`?
    bool pass(const std::vector<Match> &kept, size_t pattern_count) const;

private:
    bool active_ = false;
    Node root_;
};

// Build `scope` for one file when a scope config resolves for it. Returns
// &scope when built, nullptr otherwise; build errors go to stderr and the
// scan continues without scope annotation. When `out_cfg` is non-null it
// receives the per-file resolved config (edit mode needs the pack's
// open/close delimiters for scope-body spans).
const ScopeIndex *build_file_scope(const std::string &scope_lang,
                                   const ScopeConfig &custom,
                                   const std::string &display_name,
                                   std::string_view content,
                                   const LineIndex &idx, ScopeIndex &scope,
                                   ScopeConfig *out_cfg = nullptr);

// -in-scope / -in-scope-kind / -lines filters. Applied to `kept` AFTER
// relations (relations judge proximity against the full match set) and
// BEFORE -file-where (the predicate sees only matches that count).
class TargetFilter {
public:
    // Compile filters from CLI state; false + stderr on a bad regex.
    bool init(const Cli &cli);

    bool active() const { return active_; }
    // True when an -in-scope/-in-scope-kind filter exists (the caller must
    // then enable scope detection — `-scope auto` is implied when none is
    // configured).
    bool scope_needed() const { return scope_needed_; }
    bool lines_needed() const { return !lines_.empty(); }

    // Drop matches outside the line ranges / outside a matching enclosing
    // scope. With a scope filter active and no scope index for the file,
    // every match is dropped.
    void apply(std::vector<Match> &kept, const LineIndex &idx,
               const ScopeIndex *scope_ptr) const;

    // Does this scope itself satisfy the name/kind predicate? (Used for the
    // ancestor-chain test, anchorless edit targets, and -list-scopes.)
    bool scope_matches(const ScopeRange &r) const;

private:
    std::vector<std::regex> name_res_;
    std::string kind_;
    std::vector<Cli::LineRange> lines_;
    bool scope_needed_ = false;
    bool active_ = false;
};

// Per-file match production. Holds the run-constant pieces (patterns,
// resolved relations, -git-added-lines mode) plus a reusable raw-match
// buffer so per-file collection doesn't reallocate.
class MatchCollector {
public:
    // `patterns` is borrowed and must outlive the collector.
    MatchCollector(const std::vector<Pattern> &patterns,
                   std::vector<ResolvedRelation> rels, bool git_added_lines);

    // Produce the filtered match list for one file buffer. `added` is this
    // file's -git-added-lines entry (nullptr = no added lines; ignored
    // unless the collector was built with git_added_lines=true).
    // `scope_ptr` may be null; it is only consulted for -same-scope /
    // -not-same-scope relations. `idx` must be built whenever relations are
    // present or git line filtering is active.
    void collect(Matcher &matcher, std::string_view content,
                 const LineIndex &idx, const ScopeIndex *scope_ptr,
                 const AddedLines *added, std::vector<Match> &kept);

private:
    const std::vector<Pattern> &patterns_;
    std::vector<ResolvedRelation> rels_;
    bool git_added_ = false;
    bool any_scope_rel_ = false;
    std::vector<Match> raw_; // reused across files
};

} // namespace hpr
