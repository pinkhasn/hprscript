#include "runner.hpp"

#include "extract.hpp"
#include "file_io.hpp"
#include "git.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

// Canonical "shape" of a line: identifier-like runs collapsed to `_`,
// whitespace runs collapsed to a single space. Used by -sample to dedupe
// near-identical match contexts (e.g. `x := foo()` vs `y := bar()` → `_ := _()`).
std::string normalise_shape(std::string_view line) {
    std::string s;
    s.reserve(line.size());
    bool in_ident = false;
    bool prev_space = false;
    for (char c : line) {
        bool is_id = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        if (is_id) {
            if (!in_ident) { s += '_'; in_ident = true; prev_space = false; }
            continue;
        }
        in_ident = false;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) { s += ' '; prev_space = true; }
        } else {
            s += c;
            prev_space = false;
        }
    }
    return s;
}

bool looks_binary(std::string_view content) {
    size_t n = std::min<size_t>(content.size(), 512);
    for (size_t i = 0; i < n; ++i) {
        if (content[i] == '\0') return true;
    }
    return false;
}

// ---- -file-where expression -------------------------------------------------
// Boolean predicate over pattern ids, evaluated per file against "did this
// pattern match at least once here". Grammar:
//   expr  := and (OR|'||' and)*
//   and   := unary (AND|'&&' unary)*
//   unary := (NOT|'!') unary | '(' expr ')' | pattern-id
// Keywords are case-insensitive; ids are names, `p<i>`, or numeric indices.
struct WhereNode {
    enum Kind { And, Or, Not, Leaf } kind = Leaf;
    std::string id;    // Leaf: pattern id as written
    uint32_t pat = 0;  // Leaf: resolved pattern index
    std::vector<WhereNode> kids;
};

struct WhereParser {
    std::string_view src;
    size_t pos = 0;
    std::string err;

    void skip_ws() {
        while (pos < src.size() && std::isspace((unsigned char)src[pos])) ++pos;
    }
    bool at_end() { skip_ws(); return pos >= src.size(); }
    bool word(std::string &out) {
        skip_ws();
        size_t s = pos;
        while (pos < src.size() &&
               (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
            ++pos;
        if (pos == s) return false;
        out.assign(src.substr(s, pos - s));
        return true;
    }
    static bool is_kw(const std::string &w, const char *kw) {
        if (w.size() != std::strlen(kw)) return false;
        for (size_t i = 0; i < w.size(); ++i)
            if (std::tolower((unsigned char)w[i]) != kw[i]) return false;
        return true;
    }
    bool eat_kw(const char *kw) {
        size_t save = pos;
        std::string w;
        if (word(w) && is_kw(w, kw)) return true;
        pos = save;
        return false;
    }
    bool eat_sym(const char *sym) {
        skip_ws();
        size_t n = std::strlen(sym);
        if (src.compare(pos, std::min(n, src.size() - pos), sym) == 0 &&
            pos + n <= src.size()) {
            pos += n;
            return true;
        }
        return false;
    }

    bool parse_expr(WhereNode &out) { return parse_or(out); }
    bool parse_or(WhereNode &out) {
        WhereNode first;
        if (!parse_and(first)) return false;
        WhereNode node;
        node.kind = WhereNode::Or;
        node.kids.push_back(std::move(first));
        while (eat_sym("||") || eat_kw("or")) {
            WhereNode next;
            if (!parse_and(next)) return false;
            node.kids.push_back(std::move(next));
        }
        out = node.kids.size() == 1 ? std::move(node.kids[0]) : std::move(node);
        return true;
    }
    bool parse_and(WhereNode &out) {
        WhereNode first;
        if (!parse_unary(first)) return false;
        WhereNode node;
        node.kind = WhereNode::And;
        node.kids.push_back(std::move(first));
        while (eat_sym("&&") || eat_kw("and")) {
            WhereNode next;
            if (!parse_unary(next)) return false;
            node.kids.push_back(std::move(next));
        }
        out = node.kids.size() == 1 ? std::move(node.kids[0]) : std::move(node);
        return true;
    }
    bool parse_unary(WhereNode &out) {
        if (eat_sym("!") || eat_kw("not")) {
            WhereNode inner;
            if (!parse_unary(inner)) return false;
            out = WhereNode{};
            out.kind = WhereNode::Not;
            out.kids.push_back(std::move(inner));
            return true;
        }
        if (eat_sym("(")) {
            if (!parse_expr(out)) return false;
            if (!eat_sym(")")) { err = "expected ')'"; return false; }
            return true;
        }
        std::string w;
        if (!word(w)) { err = "expected a pattern id"; return false; }
        if (is_kw(w, "and") || is_kw(w, "or") || is_kw(w, "not")) {
            err = "misplaced keyword '" + w + "'";
            return false;
        }
        out = WhereNode{};
        out.kind = WhereNode::Leaf;
        out.id = std::move(w);
        return true;
    }
};

bool eval_where(const WhereNode &n, const std::vector<char> &matched) {
    switch (n.kind) {
        case WhereNode::Leaf: return matched[n.pat] != 0;
        case WhereNode::Not:  return !eval_where(n.kids[0], matched);
        case WhereNode::And:
            for (const auto &k : n.kids)
                if (!eval_where(k, matched)) return false;
            return true;
        case WhereNode::Or:
            for (const auto &k : n.kids)
                if (eval_where(k, matched)) return true;
            return false;
    }
    return false;
}

template <typename Resolve>
bool resolve_where_leaves(WhereNode &n, const Resolve &resolve,
                          std::string &err) {
    if (n.kind == WhereNode::Leaf) {
        if (!resolve(n.id, n.pat)) {
            err = "unknown pattern '" + n.id + "' in -file-where";
            return false;
        }
        return true;
    }
    for (auto &k : n.kids)
        if (!resolve_where_leaves(k, resolve, err)) return false;
    return true;
}

} // namespace

namespace hpr {

int run_search(const Cli &cli) {
    if (cli.patterns.empty()) {
        std::fprintf(stderr, "hprscript: -p <pattern> required\n");
        return 2;
    }
    const auto t_start = std::chrono::steady_clock::now();
    ScanStats stats;

    // Build pattern list. Per-pattern name/word_boundary/utf8 overrides come
    // from -name / -patterns-from entries; -1 means inherit the global flag.
    std::vector<Pattern> patterns;
    patterns.reserve(cli.patterns.size());
    for (size_t i = 0; i < cli.patterns.size(); ++i) {
        const CliPattern &cp = cli.patterns[i];
        Pattern p;
        p.id = cp.name.empty() ? "p" + std::to_string(i) : cp.name;
        p.regexp = cp.regexp;
        p.case_insensitive = cp.case_insensitive;
        p.word_boundary =
            cp.word_boundary < 0 ? cli.word_boundary : cp.word_boundary != 0;
        p.utf8 = cp.utf8 < 0 ? !cli.no_utf8 : cp.utf8 != 0;
        p.ucp = p.utf8 && cli.ucp;
        p.extract_names = cp.extract_names;
        patterns.push_back(std::move(p));
    }

    // Resolve relation a/b strings to pattern indices.
    struct ResolvedRelation {
        Cli::RelationKind kind;
        uint32_t a_idx, b_idx;
        int lines;
    };
    std::vector<ResolvedRelation> rels;
    auto resolve_pat = [&](const std::string &id, uint32_t &out) -> bool {
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (patterns[i].id == id) { out = static_cast<uint32_t>(i); return true; }
        }
        // Numeric form: "0", "1", …
        char *end = nullptr;
        long n = std::strtol(id.c_str(), &end, 10);
        if (end != id.c_str() && *end == '\0' && n >= 0 &&
            static_cast<size_t>(n) < patterns.size()) {
            out = static_cast<uint32_t>(n);
            return true;
        }
        return false;
    };
    for (const auto &r : cli.relations) {
        ResolvedRelation rr;
        rr.kind = r.kind;
        rr.lines = r.lines;
        if (!resolve_pat(r.a, rr.a_idx) || !resolve_pat(r.b, rr.b_idx)) {
            std::fprintf(stderr,
                         "hprscript: relation: unknown pattern in '%s:%s'\n",
                         r.a.c_str(), r.b.c_str());
            return 2;
        }
        rels.push_back(rr);
    }
    bool any_scope_rel = false;
    for (const auto &r : rels)
        if (r.kind == Cli::RelationKind::SameScope ||
            r.kind == Cli::RelationKind::NotSameScope)
            any_scope_rel = true;

    // Parse and resolve the -file-where predicate.
    WhereNode where_root;
    bool have_where = false;
    if (!cli.file_where.empty()) {
        WhereParser wp;
        wp.src = cli.file_where;
        if (!wp.parse_expr(where_root) || !wp.at_end()) {
            std::fprintf(stderr, "hprscript: -file-where: %s\n",
                         wp.err.empty() ? "unexpected trailing input"
                                        : wp.err.c_str());
            return 2;
        }
        std::string werr;
        auto rp = [&](const std::string &id, uint32_t &out) {
            return resolve_pat(id, out);
        };
        if (!resolve_where_leaves(where_root, rp, werr)) {
            std::fprintf(stderr, "hprscript: %s\n", werr.c_str());
            return 2;
        }
        have_where = true;
    }

    Matcher matcher;
    CompileError ce;
    if (!matcher.compile(patterns, &ce)) {
        std::fprintf(stderr, "hprscript: pattern compile failed: %s\n",
                     ce.message.c_str());
        if (ce.pattern_index >= 0 &&
            static_cast<size_t>(ce.pattern_index) < patterns.size()) {
            std::fprintf(stderr, "  in pattern: %s\n",
                         patterns[ce.pattern_index].regexp.c_str());
        }
        return 2;
    }

    ExtractTable extract_table;
    {
        std::string ee;
        int ee_idx = -1;
        if (!extract_table.build(patterns, &ee, &ee_idx)) {
            std::fprintf(stderr, "hprscript: %s\n", ee.c_str());
            if (ee_idx >= 0 && static_cast<size_t>(ee_idx) < patterns.size()) {
                std::fprintf(stderr, "  in pattern: %s\n",
                             patterns[ee_idx].regexp.c_str());
            }
            return 2;
        }
    }

    OutputOptions oo;
    oo.mode = cli.out_mode;
    oo.format_template = cli.format_template;
    oo.context_before = cli.context_before;
    oo.context_after = cli.context_after;
    oo.block_open = cli.block_open;
    oo.block_close = cli.block_close;
    oo.max_match_bytes = cli.max_match_bytes;
    oo.max_context_bytes = cli.max_context_bytes;
    oo.max_block_bytes = cli.max_block_bytes;
    oo.max_output_bytes = cli.max_output_bytes;
    oo.extract_table = extract_table.any() ? &extract_table : nullptr;
    oo.pattern_count = patterns.size();
    oo.global_limit = cli.limit;
    Formatter fmt(oo, stdout);
    // Block extraction populates `block_line_start`/`_end` from the line
    // index, so we need it whenever block delimiters are configured (even
    // for output modes that wouldn't otherwise build it).
    const bool need_idx = needs_line_index(oo.mode)
                           || (!oo.block_open.empty() && !oo.block_close.empty())
                           || cli.records != Cli::RecordMode::None
                           || cli.git_added_lines;

    Walker walker;
    for (const auto &g : cli.globs) walker.add_scan(g);
    for (const auto &p : cli.positional) walker.add_scan(p);
    for (const auto &e : cli.excludes) walker.add_exclude(e);

    // File-list inputs: literal paths, no glob interpretation. Missing
    // entries warn but don't fail the run — a `git diff --name-only` list
    // legitimately contains deleted files.
    for (const auto &fl : cli.file_lists) {
        std::vector<std::string> paths;
        std::string lerr;
        if (!read_path_list(fl.path, fl.nul, paths, &lerr)) {
            std::fprintf(stderr, "hprscript: %s\n", lerr.c_str());
            return 2;
        }
        for (auto &p : paths) {
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                ++stats.missing_paths;
                if (cli.diagnostics) {
                    emit_warning_record("missing_path", p);
                } else {
                    std::fprintf(stderr,
                                 "hprscript: files-from: cannot access %s\n",
                                 p.c_str());
                }
                continue;
            }
            walker.add_literal(p);
        }
    }

    // Git-selected files join the literal pipeline; missing entries (e.g.
    // deleted after the diff) warn like -files-from misses.
    GitSelection gsel{cli.git_changed, cli.git_staged, cli.git_untracked,
                      cli.git_ranges};
    if (gsel.any()) {
        std::vector<std::string> gpaths;
        std::string gerr;
        if (!git_select_files(gsel, gpaths, gerr)) {
            std::fprintf(stderr, "hprscript: git: %s\n", gerr.c_str());
            return 2;
        }
        for (auto &p : gpaths) {
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                ++stats.missing_paths;
                if (cli.diagnostics) {
                    emit_warning_record("missing_path", p);
                } else {
                    std::fprintf(stderr, "hprscript: git: cannot access %s\n",
                                 p.c_str());
                }
                continue;
            }
            walker.add_literal(p);
        }
    }
    std::unordered_map<std::string, AddedLines> added;
    if (cli.git_added_lines) {
        std::string gerr;
        if (!git_added_lines(gsel, added, gerr)) {
            std::fprintf(stderr, "hprscript: git: %s\n", gerr.c_str());
            return 2;
        }
    }

    bool no_inputs = cli.globs.empty() && cli.positional.empty() &&
                     cli.file_lists.empty() && !gsel.any();
    bool reading_stdin = no_inputs && !isatty(fileno(stdin));

    // Reused match buffer to avoid reallocating per file.
    std::vector<Match> raw;

    // Unknown pack names used to silently disable scope annotation (the
    // cookbook shipped a `-scope py` recipe nobody could tell was a no-op).
    if (!cli.scope_lang.empty() && cli.scope_lang != "auto" &&
        !builtin_scope_pack(cli.scope_lang)) {
        std::fprintf(stderr,
                     "hprscript: unknown -scope pack '%s' (supported: auto, "
                     "go, rust, c, cpp, java, js, ts)\n",
                     cli.scope_lang.c_str());
        return 2;
    }

    // Custom scope-config (used when no built-in pack is selected).
    ScopeConfig user_scope_custom;
    user_scope_custom.anchor_regex = cli.scope_pattern;
    user_scope_custom.open = cli.scope_open;
    user_scope_custom.close = cli.scope_close;
    user_scope_custom.kind = cli.scope_kind;
    bool scope_enabled = !cli.scope_lang.empty() ||
                         (!cli.scope_pattern.empty() && !cli.scope_open.empty() &&
                          !cli.scope_close.empty());
    if (any_scope_rel && !scope_enabled) {
        std::fprintf(stderr,
                     "hprscript: -same-scope/-not-same-scope require an active "
                     "-scope (built-in pack or -scope-pattern)\n");
        return 2;
    }
    if (have_where && oo.mode == OutputMode::Absent) {
        std::fprintf(stderr,
                     "hprscript: -file-where cannot combine with -absent — "
                     "express absence inside the predicate instead "
                     "(e.g. -file-where 'NOT x' -f)\n");
        return 2;
    }
    if (cli.records != Cli::RecordMode::None) {
        if (oo.mode != OutputMode::Absent) {
            std::fprintf(stderr,
                         "hprscript: -records requires -absent "
                         "(record-level absence)\n");
            return 2;
        }
        if (!rels.empty()) {
            std::fprintf(stderr,
                         "hprscript: -records cannot combine with "
                         "-near/-far/-same-scope relations\n");
            return 2;
        }
    }
    if (cli.git_added_lines) {
        if (!cli.globs.empty() || !cli.positional.empty() ||
            !cli.file_lists.empty()) {
            std::fprintf(stderr,
                         "hprscript: -git-added-lines takes its input from "
                         "git alone — drop -glob/positional paths/"
                         "-files-from\n");
            return 2;
        }
        if (cli.records != Cli::RecordMode::None) {
            std::fprintf(stderr,
                         "hprscript: -git-added-lines cannot combine with "
                         "-records\n");
            return 2;
        }
    }

    // Sample-mode buffering. When sample_n > 0 we collect kept matches into a
    // per-file table, then post-process at end to pick stratified
    // representatives. Cap matches buffered to bound memory.
    const bool sampling = cli.sample_n > 0;
    if (sampling) {
        // Sample is incompatible with output modes that don't emit per-match
        // data — let the user know rather than silently doing nothing.
        if (oo.mode == OutputMode::FilesOnly || oo.mode == OutputMode::Counts ||
            oo.mode == OutputMode::Absent) {
            std::fprintf(stderr,
                         "hprscript: -sample requires a per-match output mode (default JSONL, -o, -format)\n");
            return 2;
        }
    }
    struct SampleFile {
        std::string path;
        std::string content;
        LineIndex idx;
        ScopeIndex scope;
        bool scope_built = false;
    };
    struct SampleRec {
        size_t file_idx;
        Match m;
    };
    std::vector<SampleFile> sample_files;
    std::vector<SampleRec> sample_recs;
    const size_t SAMPLE_REC_CAP = std::max<size_t>(
        100u * static_cast<size_t>(cli.sample_n > 0 ? cli.sample_n : 1),
        10000u);

    auto scan_buf = [&](const std::string &display_name,
                        std::string_view content) -> bool {
        LineIndex idx;
        if (need_idx || scope_enabled || !rels.empty()) idx.build(content);

        ScopeIndex scope;
        const ScopeIndex *scope_ptr = nullptr;
        if (scope_enabled) {
            ScopeConfig sc = resolve_scope_for_file(cli.scope_lang,
                                                    user_scope_custom,
                                                    display_name);
            if (!sc.anchor_regex.empty()) {
                std::string serr;
                if (!scope.build(content, sc, idx, &serr)) {
                    std::fprintf(stderr, "hprscript: %s\n", serr.c_str());
                } else {
                    scope_ptr = &scope;
                }
            }
        }

        // Vectorscan reports every accepting position, so a regex like
        // `func\w+` against "func main" yields matches at to=5,6,7…10.
        // Collect raw matches and post-process to leftmost-longest
        // non-overlapping per pattern (grep-style).
        raw.clear();
        Matcher::MatchCb cb = [&](const Match &m) -> bool {
            raw.push_back(m);
            return true;
        };
        matcher.scan(content, cb);

        // Single sort-based dedup pass: by (pattern, from, -to). Within a
        // pattern, after this sort the longest match at each `from` comes
        // first, and any later match whose `from` lies before the previous
        // kept match's `to` is overlapping (skip).
        std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
            if (a.pattern_index != b.pattern_index)
                return a.pattern_index < b.pattern_index;
            if (a.from != b.from) return a.from < b.from;
            return a.to > b.to;
        });
        std::vector<Match> kept;
        kept.reserve(raw.size());
        uint32_t cur_pat = UINT32_MAX;
        uint64_t last_to = 0;
        for (const auto &m : raw) {
            if (m.pattern_index != cur_pat) {
                cur_pat = m.pattern_index;
                last_to = 0;
            }
            if (m.from < last_to) continue;
            last_to = m.to;
            kept.push_back(m);
        }
        // Final emission order: by (from, pat) globally so multi-pattern
        // results interleave sensibly.
        if (patterns.size() > 1) {
            std::sort(kept.begin(), kept.end(), [](const Match &a, const Match &b) {
                if (a.from != b.from) return a.from < b.from;
                return a.pattern_index < b.pattern_index;
            });
        }

        // -git-added-lines: only matches starting on a diff-added line
        // survive (untracked files count whole-file when selected).
        if (cli.git_added_lines && !kept.empty()) {
            auto ait = added.find(display_name);
            if (ait == added.end()) {
                kept.clear();
            } else if (!ait->second.whole_file) {
                std::vector<Match> onadd;
                onadd.reserve(kept.size());
                for (const auto &mm : kept) {
                    uint32_t L = idx.line_of(mm.from);
                    if (std::binary_search(ait->second.lines.begin(),
                                           ait->second.lines.end(), L))
                        onadd.push_back(mm);
                }
                kept = std::move(onadd);
            }
        }

        // Apply -near / -far / scope-relation filters. Rebuild a per-pattern
        // sorted line list and walk each surviving match against every
        // relation it's the `a` side of. ANDed: any failed predicate drops
        // the match.
        if (!rels.empty() && !kept.empty()) {
            std::vector<std::vector<uint32_t>> lines_by_pat(patterns.size());
            for (const auto &mm : kept)
                lines_by_pat[mm.pattern_index].push_back(idx.line_of(mm.from));
            for (auto &v : lines_by_pat) std::sort(v.begin(), v.end());
            // Scope-relation prep: innermost scope per match plus per-scope
            // pattern occurrence counts (a == b needs a second occurrence).
            std::vector<const ScopeRange *> mscope;
            std::unordered_map<const ScopeRange *, std::vector<int>> scope_pats;
            if (any_scope_rel) {
                mscope.assign(kept.size(), nullptr);
                if (scope_ptr) {
                    for (size_t ki = 0; ki < kept.size(); ++ki) {
                        mscope[ki] = scope_ptr->find_innermost(kept[ki].from);
                        if (!mscope[ki]) continue;
                        auto &v = scope_pats[mscope[ki]];
                        if (v.empty()) v.assign(patterns.size(), 0);
                        v[kept[ki].pattern_index] += 1;
                    }
                }
            }
            std::vector<Match> filtered;
            filtered.reserve(kept.size());
            for (size_t ki = 0; ki < kept.size(); ++ki) {
                const Match &mm = kept[ki];
                uint32_t mline = idx.line_of(mm.from);
                bool drop = false;
                for (const auto &r : rels) {
                    if (r.a_idx != mm.pattern_index) continue;
                    if (r.kind == Cli::RelationKind::SameScope ||
                        r.kind == Cli::RelationKind::NotSameScope) {
                        const ScopeRange *sr =
                            mscope.empty() ? nullptr : mscope[ki];
                        bool found = false;
                        if (sr) {
                            auto sit = scope_pats.find(sr);
                            if (sit != scope_pats.end()) {
                                int need = (r.a_idx == r.b_idx) ? 2 : 1;
                                found = sit->second[r.b_idx] >= need;
                            }
                        }
                        if (r.kind == Cli::RelationKind::SameScope && !found) {
                            drop = true; break;
                        }
                        if (r.kind == Cli::RelationKind::NotSameScope && found) {
                            drop = true; break;
                        }
                        continue;
                    }
                    const auto &v = lines_by_pat[r.b_idx];
                    // Lower bound for mline - r.lines (clamped at 1)
                    uint32_t lo = (mline > static_cast<uint32_t>(r.lines))
                                  ? mline - static_cast<uint32_t>(r.lines) : 1;
                    uint32_t hi = mline + static_cast<uint32_t>(r.lines);
                    auto lit = std::lower_bound(v.begin(), v.end(), lo);
                    bool found = false;
                    if (r.a_idx == r.b_idx) {
                        // Exclude self (this match's own line — counts only when
                        // the pattern's only contribution to that range is itself).
                        // We approximate: if any element in [lo,hi] differs from
                        // mline, found=true; else found requires count>1 at mline.
                        for (auto it = lit; it != v.end() && *it <= hi; ++it) {
                            if (*it != mline) { found = true; break; }
                        }
                        if (!found) {
                            // Multiple matches on same line?
                            size_t cnt = 0;
                            for (auto it = lit; it != v.end() && *it <= hi; ++it)
                                if (*it == mline) ++cnt;
                            if (cnt > 1) found = true;
                        }
                    } else {
                        for (auto it = lit; it != v.end() && *it <= hi; ++it) {
                            found = true; break;
                        }
                    }
                    if (r.kind == Cli::RelationKind::Near && !found) { drop = true; break; }
                    if (r.kind == Cli::RelationKind::Far  &&  found) { drop = true; break; }
                }
                if (!drop) filtered.push_back(mm);
            }
            kept = std::move(filtered);
        }

        stats.matches_seen += kept.size();

        // -file-where: emit this file's matches only when the predicate over
        // its matched-pattern set holds.
        if (have_where) {
            std::vector<char> matched(patterns.size(), 0);
            for (const auto &mm : kept) matched[mm.pattern_index] = 1;
            if (!eval_where(where_root, matched)) {
                fmt.on_file_end(display_name, false);
                return true;
            }
        }

        // -records line (with -absent): emit one record per non-empty line
        // lacking each pattern, then skip the per-file emission path (and the
        // Absent-mode file listing) entirely.
        if (cli.records == Cli::RecordMode::Line) {
            uint32_t nlines = idx.line_count();
            std::vector<std::vector<char>> hit(patterns.size());
            for (auto &v : hit) v.assign(nlines + 1, 0);
            for (const auto &mm : kept)
                hit[mm.pattern_index][idx.line_of(mm.from)] = 1;
            for (uint32_t L = 1; L <= nlines; ++L) {
                std::string_view text = idx.line_text(L);
                if (text.empty()) continue; // blank records are skipped
                for (size_t pi = 0; pi < patterns.size(); ++pi) {
                    if (hit[pi][L]) continue;
                    fmt.on_record_absent(display_name, patterns[pi], L, text);
                    if (fmt.over_budget()) {
                        if (stats.stop_reason.empty())
                            stats.stop_reason = "output_budget";
                        return false;
                    }
                    if (cli.limit > 0 &&
                        fmt.emitted() >= static_cast<uint64_t>(cli.limit)) {
                        fmt.mark_limit_hit();
                        if (stats.stop_reason.empty())
                            stats.stop_reason = "limit";
                        return false;
                    }
                }
            }
            return true;
        }

        bool had_match = false;
        if (sampling) {
            if (kept.empty()) return true;
            if (sample_recs.size() >= SAMPLE_REC_CAP) return true;
            // Take ownership of file content + indices the sampler will need.
            SampleFile sf;
            sf.path = display_name;
            sf.content.assign(content.data(), content.size());
            sf.idx.build(sf.content);
            if (scope_ptr) {
                // Re-build against the owned content so pointers stay valid.
                ScopeConfig sc = resolve_scope_for_file(cli.scope_lang,
                                                        user_scope_custom,
                                                        display_name);
                if (!sc.anchor_regex.empty()) {
                    std::string serr;
                    if (sf.scope.build(sf.content, sc, sf.idx, &serr)) {
                        sf.scope_built = true;
                    }
                }
            }
            size_t fidx = sample_files.size();
            sample_files.push_back(std::move(sf));
            for (const auto &m : kept) {
                if (sample_recs.size() >= SAMPLE_REC_CAP) break;
                sample_recs.push_back({fidx, m});
            }
            return true;
        }

        uint64_t per_file = 0;
        for (const auto &m : kept) {
            had_match = true;
            ++per_file;
            const Pattern &pat = patterns[m.pattern_index];
            fmt.on_match(display_name, pat, m, content, idx, scope_ptr);
            if (fmt.over_budget()) break;
            if (cli.per_file_limit > 0 &&
                per_file >= static_cast<uint64_t>(cli.per_file_limit)) break;
            if (cli.limit > 0 &&
                fmt.emitted() >= static_cast<uint64_t>(cli.limit)) break;
        }
        fmt.on_file_end(display_name, had_match);

        if (fmt.over_budget()) {
            if (stats.stop_reason.empty()) stats.stop_reason = "output_budget";
            return false;
        }
        if (cli.limit > 0 &&
            fmt.emitted() >= static_cast<uint64_t>(cli.limit)) {
            fmt.mark_limit_hit();
            if (stats.stop_reason.empty()) stats.stop_reason = "limit";
            return false;
        }
        return true;
    };

    if (reading_stdin) {
        std::string content;
        if (!read_stdin(content)) {
            std::fprintf(stderr, "hprscript: failed to read stdin\n");
            return 2;
        }
        ++stats.files_scanned;
        scan_buf("<stdin>", content);
    } else {
        walker.walk([&](const WalkItem &it) {
            MappedFile mf;
            if (!mf.open(it.path)) {
                ++stats.files_failed;
                if (cli.diagnostics) {
                    emit_warning_record("read_error", it.path);
                } else {
                    std::fprintf(stderr, "hprscript: cannot read %s: %s\n",
                                 it.path.c_str(), std::strerror(errno));
                }
                return true;
            }
            if (looks_binary(mf.view())) {
                ++stats.files_binary;
                if (cli.diagnostics) emit_warning_record("binary_skip", it.path);
                return true;
            }
            ++stats.files_scanned;
            return scan_buf(it.path, mf.view());
        });
    }

    if (sampling) {
        // Bucket records by (file_idx, shape). Order of first-seen buckets is
        // preserved per file so the round-robin emits in walk order.
        struct Bucket { size_t file_idx; std::vector<size_t> rec_indices; };
        std::vector<Bucket> buckets;
        std::map<std::pair<size_t, std::string>, size_t> bucket_of;

        for (size_t ri = 0; ri < sample_recs.size(); ++ri) {
            const auto &r = sample_recs[ri];
            const SampleFile &sf = sample_files[r.file_idx];
            uint32_t line = sf.idx.line_of(r.m.from);
            std::string_view ltext = sf.idx.line_text(line);
            std::string shape = normalise_shape(ltext.data() ? ltext : std::string_view{});
            auto key = std::make_pair(r.file_idx, shape);
            auto it = bucket_of.find(key);
            if (it == bucket_of.end()) {
                bucket_of.emplace(std::move(key), buckets.size());
                buckets.push_back(Bucket{r.file_idx, {ri}});
            } else {
                buckets[it->second].rec_indices.push_back(ri);
            }
        }

        // Round-robin per file. We keep an index into each file's bucket list
        // and advance one bucket per file per pass; within a bucket we pop one
        // record at a time and move on to the next bucket on the next visit.
        std::map<size_t, std::vector<size_t>> file_buckets;
        for (size_t bi = 0; bi < buckets.size(); ++bi)
            file_buckets[buckets[bi].file_idx].push_back(bi);

        std::vector<size_t> file_order;
        file_order.reserve(file_buckets.size());
        for (size_t i = 0; i < sample_files.size(); ++i)
            if (file_buckets.count(i)) file_order.push_back(i);

        std::map<size_t, size_t> next_bucket_in_file;
        std::map<size_t, size_t> next_rec_in_bucket;

        std::vector<size_t> selected;
        selected.reserve(static_cast<size_t>(cli.sample_n));
        size_t want = static_cast<size_t>(cli.sample_n);

        bool progress = true;
        while (selected.size() < want && progress) {
            progress = false;
            for (size_t fi : file_order) {
                if (selected.size() >= want) break;
                auto &bvec = file_buckets[fi];
                size_t &b_idx = next_bucket_in_file[fi];
                while (b_idx < bvec.size()) {
                    size_t bid = bvec[b_idx];
                    auto &recs = buckets[bid].rec_indices;
                    size_t &r_idx = next_rec_in_bucket[bid];
                    if (r_idx < recs.size()) {
                        selected.push_back(recs[r_idx++]);
                        progress = true;
                        break;
                    }
                    ++b_idx;
                }
            }
        }

        for (size_t ri : selected) {
            const auto &r = sample_recs[ri];
            const SampleFile &sf = sample_files[r.file_idx];
            const Pattern &pat = patterns[r.m.pattern_index];
            fmt.on_match(sf.path, pat, r.m, sf.content, sf.idx,
                         sf.scope_built ? &sf.scope : nullptr);
            if (fmt.over_budget()) {
                if (stats.stop_reason.empty())
                    stats.stop_reason = "output_budget";
                break;
            }
        }
    }

    fmt.on_complete();

    if (cli.summary) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
        emit_summary_record(stats, fmt.emitted(),
                            static_cast<uint64_t>(elapsed));
    }
    if (cli.require_complete &&
        (stats.files_failed > 0 || stats.missing_paths > 0)) {
        std::fprintf(stderr,
                     "hprscript: incomplete scan: %llu unreadable file(s), "
                     "%llu missing listed path(s)\n",
                     (unsigned long long)stats.files_failed,
                     (unsigned long long)stats.missing_paths);
        return 2;
    }

    // Exit code semantics follow grep: 0 if any match emitted (or absent
    // files printed), 1 if no output, 2 already returned earlier on errors.
    return fmt.emitted() > 0 ? 0 : 1;
}

} // namespace hpr
