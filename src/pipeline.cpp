#include "pipeline.hpp"

#include "file_io.hpp"
#include "output.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace {

using WhereNode = hpr::FileWhere::Node;
using WhereOp = hpr::FileWhere::Op;

// ---- -file-where expression -------------------------------------------------
// Grammar:
//   expr  := and (OR|'||' and)*
//   and   := unary (AND|'&&' unary)*
//   unary := (NOT|'!') unary | '(' expr ')' | leaf
//   leaf  := ident '(' [arg] ')' cmp number    -- churn(30) > 2, count(p0) >= 3
//          | 'lang' cmp ident                  -- lang == go
//          | ident                             -- bare pattern id (presence)
//   cmp   := '>=' | '<=' | '==' | '!=' | '>' | '<'
// Keywords are case-insensitive; pattern ids are names, `p<i>`/`ident<i>`,
// or numeric indices.
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
    // Longest-symbol-first so ">=" isn't swallowed as "> ' '='".
    bool parse_cmp_op(WhereOp &op) {
        skip_ws();
        if (eat_sym(">=")) { op = WhereOp::Ge; return true; }
        if (eat_sym("<=")) { op = WhereOp::Le; return true; }
        if (eat_sym("=="))  { op = WhereOp::Eq; return true; }
        if (eat_sym("!="))  { op = WhereOp::Ne; return true; }
        if (eat_sym(">"))   { op = WhereOp::Gt; return true; }
        if (eat_sym("<"))   { op = WhereOp::Lt; return true; }
        return false;
    }
    bool parse_number(double &out) {
        skip_ws();
        size_t s = pos;
        size_t p = pos;
        if (p < src.size() && (src[p] == '-' || src[p] == '+')) ++p;
        bool any_digit = false;
        while (p < src.size() && std::isdigit((unsigned char)src[p])) { ++p; any_digit = true; }
        if (p < src.size() && src[p] == '.') {
            ++p;
            while (p < src.size() && std::isdigit((unsigned char)src[p])) { ++p; any_digit = true; }
        }
        if (!any_digit) return false;
        out = std::strtod(std::string(src.substr(s, p - s)).c_str(), nullptr);
        pos = p;
        return true;
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
        if (!word(w)) { err = "expected a pattern id or condition"; return false; }
        if (is_kw(w, "and") || is_kw(w, "or") || is_kw(w, "not")) {
            err = "misplaced keyword '" + w + "'";
            return false;
        }

        // Function-style condition: churn(N) OP number | count(pat) OP number.
        skip_ws();
        if (pos < src.size() && src[pos] == '(') {
            ++pos;
            std::string arg;
            skip_ws();
            if (pos < src.size() && src[pos] != ')') {
                if (!word(arg)) {
                    err = "expected an argument inside " + w + "(...)";
                    return false;
                }
            }
            if (!eat_sym(")")) { err = "expected ')' after " + w + "(...)"; return false; }
            WhereOp op;
            if (!parse_cmp_op(op)) {
                err = w + "(...) must be followed by a comparison "
                          "(>, <, >=, <=, ==, !=)";
                return false;
            }
            double num;
            if (!parse_number(num)) {
                err = "expected a number after " + w + "(...) " "comparison";
                return false;
            }
            out = WhereNode{};
            out.kind = WhereNode::Leaf;
            out.op = op;
            out.num_rhs = num;
            if (is_kw(w, "churn")) {
                char *endp = nullptr;
                long days = arg.empty() ? 0 : std::strtol(arg.c_str(), &endp, 10);
                if (arg.empty() || endp != arg.c_str() + arg.size() || days <= 0) {
                    err = "churn(...) needs a positive integer day-window, "
                          "e.g. churn(30)";
                    return false;
                }
                out.leaf_kind = WhereNode::Churn;
                out.churn_days = static_cast<int>(days);
            } else if (is_kw(w, "count")) {
                if (arg.empty()) {
                    err = "count(...) needs a pattern id, e.g. count(p0)";
                    return false;
                }
                out.leaf_kind = WhereNode::Count;
                out.id = arg;
            } else {
                err = "unknown condition '" + w +
                      "(...)' (supported: churn, count)";
                return false;
            }
            return true;
        }

        // Bare-ident comparison: currently just `lang == go` / `lang != go`.
        {
            size_t save = pos;
            WhereOp op;
            if (parse_cmp_op(op)) {
                if (!is_kw(w, "lang")) {
                    err = "unknown condition field '" + w +
                          "' (supported: lang)";
                    return false;
                }
                if (op != WhereOp::Eq && op != WhereOp::Ne) {
                    err = "lang only supports == and !=";
                    return false;
                }
                std::string rhs;
                if (!word(rhs)) {
                    err = "expected a language name after 'lang " +
                          std::string(op == WhereOp::Eq ? "==" : "!=") + "'";
                    return false;
                }
                for (auto &c : rhs) c = static_cast<char>(std::tolower((unsigned char)c));
                out = WhereNode{};
                out.kind = WhereNode::Leaf;
                out.leaf_kind = WhereNode::Lang;
                out.op = op;
                out.str_rhs = std::move(rhs);
                return true;
            }
            pos = save;
        }

        out = WhereNode{};
        out.kind = WhereNode::Leaf;
        out.leaf_kind = WhereNode::PatternPresent;
        out.id = std::move(w);
        return true;
    }
};

bool compare_num(double v, WhereOp op, double rhs) {
    switch (op) {
        case WhereOp::Gt: return v > rhs;
        case WhereOp::Lt: return v < rhs;
        case WhereOp::Ge: return v >= rhs;
        case WhereOp::Le: return v <= rhs;
        case WhereOp::Eq: return v == rhs;
        case WhereOp::Ne: return v != rhs;
    }
    return false;
}

bool eval_where(const WhereNode &n, const std::vector<char> &matched,
               const std::vector<uint32_t> &counts, const std::string &file,
               const std::map<int, std::unordered_map<std::string, uint32_t>> &churn,
               const std::string &lang) {
    switch (n.kind) {
        case WhereNode::Leaf:
            switch (n.leaf_kind) {
                case WhereNode::PatternPresent:
                    return matched[n.pat] != 0;
                case WhereNode::Count:
                    return compare_num(static_cast<double>(counts[n.pat]), n.op,
                                       n.num_rhs);
                case WhereNode::Churn: {
                    double v = 0.0;
                    auto wit = churn.find(n.churn_days);
                    if (wit != churn.end()) {
                        auto fit = wit->second.find(file);
                        if (fit != wit->second.end())
                            v = static_cast<double>(fit->second);
                    }
                    return compare_num(v, n.op, n.num_rhs);
                }
                case WhereNode::Lang: {
                    bool eq = lang == n.str_rhs;
                    return n.op == WhereOp::Eq ? eq : !eq;
                }
            }
            return false;
        case WhereNode::Not:
            return !eval_where(n.kids[0], matched, counts, file, churn, lang);
        case WhereNode::And:
            for (const auto &k : n.kids)
                if (!eval_where(k, matched, counts, file, churn, lang)) return false;
            return true;
        case WhereNode::Or:
            for (const auto &k : n.kids)
                if (eval_where(k, matched, counts, file, churn, lang)) return true;
            return false;
    }
    return false;
}

template <typename Resolve>
bool resolve_where_leaves(WhereNode &n, const Resolve &resolve,
                          std::string &err) {
    if (n.kind == WhereNode::Leaf) {
        if (n.leaf_kind == WhereNode::PatternPresent ||
            n.leaf_kind == WhereNode::Count) {
            if (!resolve(n.id, n.pat)) {
                err = "unknown pattern '" + n.id + "' in -file-where";
                return false;
            }
        }
        return true;
    }
    for (auto &k : n.kids)
        if (!resolve_where_leaves(k, resolve, err)) return false;
    return true;
}

void collect_churn_windows(const WhereNode &n, std::vector<int> &out) {
    if (n.kind == WhereNode::Leaf) {
        if (n.leaf_kind == WhereNode::Churn &&
            std::find(out.begin(), out.end(), n.churn_days) == out.end())
            out.push_back(n.churn_days);
        return;
    }
    for (const auto &k : n.kids) collect_churn_windows(k, out);
}

} // namespace

namespace hpr {

bool looks_binary(std::string_view content) {
    size_t n = std::min<size_t>(content.size(), 512);
    for (size_t i = 0; i < n; ++i) {
        if (content[i] == '\0') return true;
    }
    return false;
}

bool add_walker_inputs(const Cli &cli, Walker &walker, ScanStats &stats,
                       std::unordered_map<std::string, AddedLines> &added,
                       const std::function<void(const char *, const std::string &)> &diagnostic) {
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
            return false;
        }
        for (auto &p : paths) {
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                ++stats.missing_paths;
                if (cli.diagnostics) {
                    if (diagnostic) diagnostic("missing_path", p);
                    else emit_warning_record("missing_path", p);
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
            return false;
        }
        for (auto &p : gpaths) {
            std::error_code ec;
            if (!std::filesystem::exists(p, ec)) {
                ++stats.missing_paths;
                if (cli.diagnostics) {
                    if (diagnostic) diagnostic("missing_path", p);
                    else emit_warning_record("missing_path", p);
                } else {
                    std::fprintf(stderr, "hprscript: git: cannot access %s\n",
                                 p.c_str());
                }
                continue;
            }
            walker.add_literal(p);
        }
    }
    if (cli.git_added_lines) {
        std::string gerr;
        if (!git_added_lines(gsel, added, gerr)) {
            std::fprintf(stderr, "hprscript: git: %s\n", gerr.c_str());
            return false;
        }
    }
    return true;
}

std::vector<Pattern> build_patterns(const Cli &cli) {
    // Per-pattern name/word_boundary/utf8 overrides come from -name /
    // -patterns-from entries; -1 means inherit the global flag.
    //
    // Two passes, not one: regex-backed patterns (p0/p1/...) always end up
    // as the vector's prefix and -ident groups (ident0/ident1/...) as its
    // suffix, regardless of how the user interleaved -p/-ident on the
    // command line. That ordering is what lets Matcher compile just the
    // prefix while its reported pattern ids still index correctly into
    // this vector (see run_search() in runner.cpp) — and as a side effect,
    // adding an -ident group never renumbers existing -p patterns' ids.
    std::vector<Pattern> patterns;
    patterns.reserve(cli.patterns.size());
    size_t regex_i = 0;
    for (const auto &cp : cli.patterns) {
        if (!cp.ident_terms.empty()) continue;
        Pattern p;
        p.id = cp.name.empty() ? "p" + std::to_string(regex_i) : cp.name;
        p.regexp = cp.regexp;
        p.case_insensitive = cp.case_insensitive;
        p.word_boundary =
            cp.word_boundary < 0 ? cli.word_boundary : cp.word_boundary != 0;
        p.utf8 = cp.utf8 < 0 ? !cli.no_utf8 : cp.utf8 != 0;
        p.ucp = p.utf8 && cli.ucp;
        p.extract_names = cp.extract_names;
        p.ref = cp.ref;
        p.desc = cp.desc;
        patterns.push_back(std::move(p));
        ++regex_i;
    }
    size_t ident_i = 0;
    for (const auto &cp : cli.patterns) {
        if (cp.ident_terms.empty()) continue;
        Pattern p;
        p.id = cp.name.empty() ? "ident" + std::to_string(ident_i) : cp.name;
        // regexp deliberately left empty — ident-backed patterns never
        // reach Matcher::compile.
        p.desc = cp.desc;
        patterns.push_back(std::move(p));
        ++ident_i;
    }
    return patterns;
}

bool resolve_pattern_id(const std::vector<Pattern> &patterns,
                        const std::string &id, uint32_t &out) {
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
}

bool resolve_relations(const std::vector<Cli::Relation> &relations,
                       const std::vector<Pattern> &patterns,
                       std::vector<ResolvedRelation> &out) {
    for (const auto &r : relations) {
        ResolvedRelation rr;
        rr.kind = r.kind;
        rr.lines = r.lines;
        if (!resolve_pattern_id(patterns, r.a, rr.a_idx) ||
            !resolve_pattern_id(patterns, r.b, rr.b_idx)) {
            std::fprintf(stderr,
                         "hprscript: relation: unknown pattern in '%s:%s'\n",
                         r.a.c_str(), r.b.c_str());
            return false;
        }
        out.push_back(rr);
    }
    return true;
}

bool any_scope_relation(const std::vector<ResolvedRelation> &rels) {
    for (const auto &r : rels)
        if (r.kind == Cli::RelationKind::SameScope ||
            r.kind == Cli::RelationKind::NotSameScope)
            return true;
    return false;
}

bool FileWhere::init(const std::string &expr,
                     const std::vector<Pattern> &patterns) {
    if (expr.empty()) return true;
    WhereParser wp;
    wp.src = expr;
    if (!wp.parse_expr(root_) || !wp.at_end()) {
        std::fprintf(stderr, "hprscript: -file-where: %s\n",
                     wp.err.empty() ? "unexpected trailing input"
                                    : wp.err.c_str());
        return false;
    }
    std::string werr;
    auto rp = [&](const std::string &id, uint32_t &out) {
        return resolve_pattern_id(patterns, id, out);
    };
    if (!resolve_where_leaves(root_, rp, werr)) {
        std::fprintf(stderr, "hprscript: %s\n", werr.c_str());
        return false;
    }
    collect_churn_windows(root_, churn_windows_);
    active_ = true;
    return true;
}

bool FileWhere::pass(
    const std::vector<Match> &kept, size_t pattern_count,
    const std::string &file,
    const std::map<int, std::unordered_map<std::string, uint32_t>> &churn) const {
    std::vector<char> matched(pattern_count, 0);
    std::vector<uint32_t> counts(pattern_count, 0);
    for (const auto &mm : kept) {
        matched[mm.pattern_index] = 1;
        ++counts[mm.pattern_index];
    }
    std::string lang = auto_lang_for_path(file);
    for (auto &c : lang) c = static_cast<char>(std::tolower((unsigned char)c));
    return eval_where(root_, matched, counts, file, churn, lang);
}

bool build_churn_map(
    const std::vector<int> &windows,
    std::map<int, std::unordered_map<std::string, uint32_t>> &out,
    std::string &err) {
    for (int days : windows) {
        std::unordered_map<std::string, uint32_t> m;
        if (!git_churn(days, m, err)) return false;
        out[days] = std::move(m);
    }
    return true;
}

const ScopeIndex *build_file_scope(const std::string &scope_lang,
                                   const ScopeConfig &custom,
                                   const std::string &display_name,
                                   std::string_view content,
                                   const LineIndex &idx, ScopeIndex &scope,
                                   ScopeConfig *out_cfg) {
    ScopeConfig sc = resolve_scope_for_file(scope_lang, custom, display_name);
    if (out_cfg) *out_cfg = sc;
    if (sc.anchor_regex.empty()) return nullptr;
    std::string serr;
    if (!scope.build(content, sc, idx, &serr)) {
        std::fprintf(stderr, "hprscript: %s\n", serr.c_str());
        return nullptr;
    }
    return &scope;
}

bool TargetFilter::init(const Cli &cli) {
    kind_ = cli.in_scope_kind;
    lines_ = cli.line_ranges;
    scope_needed_ = !cli.in_scopes.empty() || !kind_.empty();
    active_ = scope_needed_ || !lines_.empty();
    for (const auto &re : cli.in_scopes) {
        try {
            name_res_.emplace_back(re, std::regex::ECMAScript);
        } catch (const std::regex_error &e) {
            std::fprintf(stderr, "hprscript: -in-scope '%s': %s\n", re.c_str(),
                         e.what());
            return false;
        }
    }
    return true;
}

bool TargetFilter::scope_matches(const ScopeRange &r) const {
    if (!kind_.empty() && r.kind != kind_) return false;
    if (name_res_.empty()) return true;
    for (const auto &re : name_res_)
        if (std::regex_search(r.name, re)) return true;
    return false;
}

void TargetFilter::apply(std::vector<Match> &kept, const LineIndex &idx,
                         const ScopeIndex *scope_ptr) const {
    if (!active_ || kept.empty()) return;
    std::vector<Match> out;
    out.reserve(kept.size());
    for (const Match &m : kept) {
        if (!lines_.empty()) {
            uint32_t line = idx.line_of(m.from);
            bool in_range = false;
            for (const auto &r : lines_) {
                if (line >= r.lo && line <= r.hi) { in_range = true; break; }
            }
            if (!in_range) continue;
        }
        if (scope_needed_) {
            // Ancestor-chain test: any containing scope (not just the
            // innermost) may satisfy the filter — code in a method of
            // `class Foo` counts as inside Foo.
            bool ok = false;
            if (scope_ptr) {
                for (const ScopeRange &r : scope_ptr->all()) {
                    if (r.start_off > m.from) break;
                    if (m.from >= r.end_off) continue;
                    if (scope_matches(r)) { ok = true; break; }
                }
            }
            if (!ok) continue;
        }
        out.push_back(m);
    }
    kept = std::move(out);
}

MatchCollector::MatchCollector(const std::vector<Pattern> &patterns,
                               std::vector<ResolvedRelation> rels,
                               bool git_added_lines,
                               std::vector<IdentGroup> ident_groups)
    : patterns_(patterns), rels_(std::move(rels)), git_added_(git_added_lines),
      ident_groups_(std::move(ident_groups)) {
    any_scope_rel_ = any_scope_relation(rels_);
    ident_base_ = static_cast<uint32_t>(patterns_.size() - ident_groups_.size());
}

void MatchCollector::collect(Matcher &matcher, std::string_view content,
                             const LineIndex &idx, const ScopeIndex *scope_ptr,
                             const AddedLines *added,
                             std::vector<Match> &kept) {
    // Vectorscan reports every accepting position, so a regex like
    // `func\w+` against "func main" yields matches at to=5,6,7…10.
    // Collect raw matches and post-process to leftmost-longest
    // non-overlapping per pattern (grep-style).
    raw_.clear();
    Matcher::MatchCb cb = [&](const Match &m) -> bool {
        raw_.push_back(m);
        return true;
    };
    matcher.scan(content, cb);
    if (!ident_groups_.empty())
        scan_identifiers(content, ident_groups_, ident_base_, raw_);

    // Single sort-based dedup pass: by (pattern, from, -to). Within a
    // pattern, after this sort the longest match at each `from` comes
    // first, and any later match whose `from` lies before the previous
    // kept match's `to` is overlapping (skip).
    std::sort(raw_.begin(), raw_.end(), [](const Match &a, const Match &b) {
        if (a.pattern_index != b.pattern_index)
            return a.pattern_index < b.pattern_index;
        if (a.from != b.from) return a.from < b.from;
        return a.to > b.to;
    });
    kept.clear();
    kept.reserve(raw_.size());
    uint32_t cur_pat = UINT32_MAX;
    uint64_t last_to = 0;
    for (const auto &m : raw_) {
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
    if (patterns_.size() > 1) {
        std::sort(kept.begin(), kept.end(), [](const Match &a, const Match &b) {
            if (a.from != b.from) return a.from < b.from;
            return a.pattern_index < b.pattern_index;
        });
    }

    // -git-added-lines: only matches starting on a diff-added line
    // survive (untracked files count whole-file when selected).
    if (git_added_ && !kept.empty()) {
        if (!added) {
            kept.clear();
        } else if (!added->whole_file) {
            std::vector<Match> onadd;
            onadd.reserve(kept.size());
            for (const auto &mm : kept) {
                uint32_t L = idx.line_of(mm.from);
                if (std::binary_search(added->lines.begin(),
                                       added->lines.end(), L))
                    onadd.push_back(mm);
            }
            kept = std::move(onadd);
        }
    }

    // Apply -near / -far / scope-relation filters. Rebuild a per-pattern
    // sorted line list and walk each surviving match against every
    // relation it's the `a` side of. ANDed: any failed predicate drops
    // the match.
    if (!rels_.empty() && !kept.empty()) {
        std::vector<std::vector<uint32_t>> lines_by_pat(patterns_.size());
        for (const auto &mm : kept)
            lines_by_pat[mm.pattern_index].push_back(idx.line_of(mm.from));
        for (auto &v : lines_by_pat) std::sort(v.begin(), v.end());
        // Scope-relation prep: innermost scope per match plus per-scope
        // pattern occurrence counts (a == b needs a second occurrence).
        std::vector<const ScopeRange *> mscope;
        std::unordered_map<const ScopeRange *, std::vector<int>> scope_pats;
        if (any_scope_rel_) {
            mscope.assign(kept.size(), nullptr);
            if (scope_ptr) {
                for (size_t ki = 0; ki < kept.size(); ++ki) {
                    mscope[ki] = scope_ptr->find_innermost(kept[ki].from);
                    if (!mscope[ki]) continue;
                    auto &v = scope_pats[mscope[ki]];
                    if (v.empty()) v.assign(patterns_.size(), 0);
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
            for (const auto &r : rels_) {
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
}

} // namespace hpr
