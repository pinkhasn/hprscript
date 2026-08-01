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
#include "ident.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <cstdint>
#include <map>
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
// -w / -no-utf8 / -ucp defaults. Regex-backed patterns (-p/-pi/-F/-Fi/
// -patterns-from) always come first, in order, auto-numbered p0/p1/...;
// -ident groups follow, auto-numbered ident0/ident1/... independently —
// this ordering is load-bearing: it's what lets Matcher compile just the
// regex prefix while Vectorscan's reported pattern ids still line up with
// this vector's indices (see run_search() in runner.cpp).
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

// -file-where: boolean predicate over pattern ids and file metadata,
// evaluated per file. Leaves are either bare pattern ids ("did this pattern
// match at least once here") or comparisons: `count(pat) >= 3` (occurrence
// count), `churn(30) > 2` (commits touching the file in the last 30 days —
// see git_churn()), or `lang == go` (auto_lang_for_path()'s guess).
class FileWhere {
public:
    enum class Op { Gt, Lt, Ge, Le, Eq, Ne };

    // Expression tree node. Public so the parser (pipeline.cpp) can build
    // trees; not intended for use outside this module.
    struct Node {
        enum Kind { And, Or, Not, Leaf } kind = Leaf;
        enum LeafKind { PatternPresent, Count, Churn, Lang } leaf_kind = PatternPresent;
        std::string id;    // PatternPresent/Count: pattern id as written
        uint32_t pat = 0;  // PatternPresent/Count: resolved pattern index
        Op op = Op::Gt;    // Count/Churn/Lang: the comparison operator
        double num_rhs = 0;      // Count/Churn: right-hand side
        int churn_days = 0;      // Churn: the N inside churn(N)
        std::string str_rhs;     // Lang: right-hand side, lowercased
        std::vector<Node> kids;
    };

    // Parse `expr` and resolve its leaves against `patterns`. An empty expr
    // leaves the predicate inactive and succeeds. On error, prints the usual
    // stderr message and returns false.
    bool init(const std::string &expr, const std::vector<Pattern> &patterns);

    bool active() const { return active_; }

    // Distinct churn(N) day-windows referenced in the predicate (empty if
    // none). The caller (runner.cpp/edit.cpp) should populate a churn map
    // for each — see build_churn_map() — before calling pass().
    const std::vector<int> &churn_windows() const { return churn_windows_; }

    // Does the predicate hold for a file whose surviving matches are `kept`?
    // `churn` may be empty when churn_windows() is empty; a file/window
    // missing from it is treated as churn 0.
    bool pass(const std::vector<Match> &kept, size_t pattern_count,
             const std::string &file,
             const std::map<int, std::unordered_map<std::string, uint32_t>> &churn = {}) const;

private:
    bool active_ = false;
    Node root_;
    std::vector<int> churn_windows_;
};

// Run git_churn() once per distinct window in `windows`, populating `out`.
// Shared by run_search() and the edit runner so both -file-where users get
// identical churn semantics from one implementation.
bool build_churn_map(const std::vector<int> &windows,
                     std::map<int, std::unordered_map<std::string, uint32_t>> &out,
                     std::string &err);

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
    // `patterns` is borrowed and must outlive the collector. `ident_groups`
    // (default empty) are the -ident groups occupying `patterns`' tail —
    // see build_patterns(); their matches come from scan_identifiers()
    // (src/ident.hpp), not Vectorscan.
    MatchCollector(const std::vector<Pattern> &patterns,
                   std::vector<ResolvedRelation> rels, bool git_added_lines,
                   std::vector<IdentGroup> ident_groups = {});

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
    std::vector<IdentGroup> ident_groups_;
    uint32_t ident_base_ = 0; // patterns_ index where ident groups start
    std::vector<Match> raw_; // reused across files
};

} // namespace hpr
