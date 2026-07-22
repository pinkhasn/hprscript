#include "scope.hpp"

#include "block.hpp"
#include "common.hpp"
#include "matcher.hpp"

#include <algorithm>
#include <regex>

namespace hpr {

namespace {

// Built-in language packs. Anchor regexes are intentionally simple: anything
// fancy goes through the user's `-scope-pattern`. Each pack's first capture
// group is the scope name.
const ScopeConfig PACK_GO    = { "func\\s+(?:\\([^)]*\\)\\s+)?(\\w+)\\s*\\(",        "{", "}", "func",  {} };
const ScopeConfig PACK_RUST  = { "fn\\s+(\\w+)",                                       "{", "}", "fn",    {} };
const ScopeConfig PACK_C     = { "\\b([a-zA-Z_]\\w*)\\s*\\([^;{}]*\\)\\s*\\{",          "{", "}", "func",
                                 {"if", "for", "while", "switch", "return", "sizeof"} };
const ScopeConfig PACK_CPP   = { "\\b([a-zA-Z_]\\w*)\\s*\\([^;{}]*\\)\\s*(?:const|noexcept|override|final|[\\s])*\\{", "{", "}", "func",
                                 {"if", "for", "while", "switch", "return", "sizeof", "catch"} };
const ScopeConfig PACK_JAVA  = { "\\b([a-zA-Z_]\\w*)\\s*\\([^;{}]*\\)\\s*(?:throws[^{]*)?\\{", "{", "}", "method",
                                 {"if", "for", "while", "switch", "return", "catch", "synchronized"} };
const ScopeConfig PACK_JS    = { "(?:function\\s+(\\w+)|class\\s+(\\w+))",             "{", "}", "func",  {} };
const ScopeConfig PACK_TS    = { "(?:function\\s+(\\w+)|class\\s+(\\w+)|method\\s+(\\w+))", "{", "}", "func", {} };

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

const ScopeConfig *builtin_scope_pack(const std::string &lang) {
    if (lang == "go")   return &PACK_GO;
    if (lang == "rust") return &PACK_RUST;
    if (lang == "c")    return &PACK_C;
    if (lang == "cpp" || lang == "c++" || lang == "cc") return &PACK_CPP;
    if (lang == "java") return &PACK_JAVA;
    if (lang == "js")   return &PACK_JS;
    if (lang == "ts")   return &PACK_TS;
    return nullptr;
}

std::string auto_lang_for_path(const std::string &path) {
    if (ends_with(path, ".go"))  return "go";
    if (ends_with(path, ".rs"))  return "rust";
    if (ends_with(path, ".c") || ends_with(path, ".h"))   return "c";
    if (ends_with(path, ".cpp") || ends_with(path, ".cc") ||
        ends_with(path, ".cxx") || ends_with(path, ".hpp") ||
        ends_with(path, ".hh")  || ends_with(path, ".hxx")) return "cpp";
    if (ends_with(path, ".java")) return "java";
    if (ends_with(path, ".mjs") || ends_with(path, ".cjs") ||
        ends_with(path, ".js"))   return "js";
    if (ends_with(path, ".tsx") || ends_with(path, ".ts")) return "ts";
    return "";
}

ScopeConfig resolve_scope_for_file(const std::string &lang,
                                   const ScopeConfig &custom,
                                   const std::string &path) {
    // Custom config wins outright.
    if (!custom.anchor_regex.empty() && !custom.open.empty() && !custom.close.empty()) {
        return custom;
    }
    if (lang.empty()) return ScopeConfig{};
    std::string resolved = lang;
    if (resolved == "auto") resolved = auto_lang_for_path(path);
    if (resolved.empty()) return ScopeConfig{};
    const ScopeConfig *pack = builtin_scope_pack(resolved);
    if (!pack) return ScopeConfig{};
    return *pack;
}

bool ScopeIndex::build(std::string_view buf, const ScopeConfig &cfg,
                       const LineIndex &idx, std::string *err) {
    ranges_.clear();
    if (cfg.anchor_regex.empty() || cfg.open.empty() || cfg.close.empty())
        return true;

    // Anchor scan via Vectorscan (fast multi-pattern engine, even for one
    // pattern — gives us the same UTF-8/multiline semantics as the rest of
    // the tool).
    Pattern p;
    p.id = "_scope";
    p.regexp = cfg.anchor_regex;
    p.utf8 = true;
    p.case_insensitive = false;

    Matcher m;
    CompileError ce;
    if (!m.compile({p}, &ce)) {
        if (err) *err = "scope anchor compile failed: " + ce.message;
        return false;
    }

    // Pre-compile std::regex once for capture extraction over each match.
    std::regex name_re;
    try {
        name_re = std::regex(cfg.anchor_regex, std::regex::ECMAScript);
    } catch (const std::regex_error &e) {
        if (err) *err = std::string("scope anchor std::regex compile failed: ") + e.what();
        return false;
    }

    std::vector<Match> raw;
    m.scan(buf, [&](const Match &mm) -> bool {
        raw.push_back(mm);
        return true;
    });

    // Vectorscan reports every accepting position; same dedup as the main
    // scanner: keep leftmost-longest per pattern.
    std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
        if (a.from != b.from) return a.from < b.from;
        return a.to > b.to;
    });
    uint64_t last_to = 0;
    std::vector<Match> kept;
    kept.reserve(raw.size());
    for (const auto &mm : raw) {
        if (mm.from < last_to) continue;
        last_to = mm.to;
        kept.push_back(mm);
    }

    ranges_.reserve(kept.size());
    for (const auto &mm : kept) {
        // Pull capture group 1 (or the first non-empty group, for multi-arm
        // patterns like JS `function|class`) out of the matched text.
        const char *match_begin = buf.data() + mm.from;
        const char *match_end = buf.data() + mm.to;
        std::cmatch cm;
        std::string name;
        if (std::regex_search(match_begin, match_end, cm, name_re)) {
            for (size_t g = 1; g < cm.size(); ++g) {
                if (cm[g].matched && cm[g].length() > 0) {
                    name.assign(cm[g].first, cm[g].second);
                    break;
                }
            }
        }
        if (std::find(cfg.skip_names.begin(), cfg.skip_names.end(), name) !=
            cfg.skip_names.end())
            continue;
        // Find the body block from the match start — the C/C++/Java anchors
        // end in `\{`, so the body's opener is part of the match itself.
        uint64_t op = 0, cp = 0;
        if (!find_balanced_block(buf, mm.from, cfg.open, cfg.close, op, cp))
            continue;
        ScopeRange r;
        r.start_off = mm.from;
        r.end_off = cp;
        r.name = std::move(name);
        r.kind = cfg.kind;
        r.line_start = idx.line_of(mm.from);
        r.line_end = idx.line_of(cp == 0 ? 0 : cp - 1);
        ranges_.push_back(std::move(r));
    }

    // Sort ranges by start ascending — inner (later-starting) ranges naturally
    // appear after their enclosing range with the same or larger start. For
    // innermost lookup we walk the array and keep the deepest match.
    std::sort(ranges_.begin(), ranges_.end(),
              [](const ScopeRange &a, const ScopeRange &b) {
                  if (a.start_off != b.start_off) return a.start_off < b.start_off;
                  return a.end_off > b.end_off; // outer-first when starts tie
              });
    return true;
}

const ScopeRange *ScopeIndex::find_innermost(uint64_t offset) const {
    // Linear scan picks the smallest-by-end range that contains offset. Could
    // be tightened with an interval tree, but typical files have <1k scopes
    // and this runs once per match (O(scopes) per match → fine in practice).
    const ScopeRange *best = nullptr;
    uint64_t best_span = UINT64_MAX;
    for (const auto &r : ranges_) {
        if (r.start_off > offset) break; // sorted by start
        if (offset >= r.end_off) continue;
        uint64_t span = r.end_off - r.start_off;
        if (span < best_span) {
            best = &r;
            best_span = span;
        }
    }
    return best;
}

} // namespace hpr
