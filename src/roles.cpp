#include "roles.hpp"

#include "scope.hpp"

#include <algorithm>
#include <cstring>

namespace hpr {

namespace {

const RoleConfig CFG_GO{
    "//", false, "/*", "*/", false, "\"'", "`", false,
    {"import ", "import ("}};
// Rust: `'` is excluded from string delims — `'a` lifetimes have no closing
// quote and would poison everything after them. Double-quote strings span
// lines; block comments nest.
const RoleConfig CFG_RUST{
    "//", false, "/*", "*/", true, "", "\"", false,
    {"use ", "extern crate "}};
const RoleConfig CFG_C{
    "//", false, "/*", "*/", false, "\"'", "", false, {"#include"}, true};
const RoleConfig CFG_JAVA{
    "//", false, "/*", "*/", false, "\"'", "", false, {"import "}};
const RoleConfig CFG_JS{
    "//", false, "/*", "*/", false, "\"'", "`", false, {"import "}};
const RoleConfig CFG_PY{
    "#", false, nullptr, nullptr, false, "\"'", "", true,
    {"import ", "from "}};
const RoleConfig CFG_SH{
    "#", true, nullptr, nullptr, false, "\"'", "", false, {}};
const RoleConfig CFG_RB{
    "#", false, nullptr, nullptr, false, "\"'", "", false,
    {"require ", "require_relative "}};
const RoleConfig CFG_HASH{ // yaml / toml: comments + quoted scalars only
    "#", false, nullptr, nullptr, false, "\"'", "", false, {}};

bool ends_with(const std::string &s, const char *suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

const RoleConfig *role_config_for_path(const std::string &path) {
    const std::string lang = auto_lang_for_path(path);
    if (lang == "go") return &CFG_GO;
    if (lang == "rust") return &CFG_RUST;
    if (lang == "c" || lang == "cpp") return &CFG_C;
    if (lang == "java") return &CFG_JAVA;
    if (lang == "js" || lang == "ts") return &CFG_JS;
    if (ends_with(path, ".py")) return &CFG_PY;
    if (ends_with(path, ".sh") || ends_with(path, ".bash")) return &CFG_SH;
    if (ends_with(path, ".rb")) return &CFG_RB;
    if (ends_with(path, ".yml") || ends_with(path, ".yaml") ||
        ends_with(path, ".toml"))
        return &CFG_HASH;
    return nullptr;
}

void RoleIndex::build(std::string_view buf, const RoleConfig &cfg,
                      const LineIndex &idx) {
    spans_.clear();
    import_lines_.clear();

    enum class St { Code, LineComment, BlockComment, Str };
    St st = St::Code;
    uint64_t span_start = 0;
    int block_depth = 0;
    char quote = 0;
    bool raw = false;       // no backslash escapes (Go/JS backtick)
    bool multiline = false; // literal may cross newlines
    bool triple = false;    // delimiter is quote×3 (Python)

    const size_t n = buf.size();
    auto starts = [&](size_t i, const char *tok) {
        if (!tok) return false;
        size_t tl = std::strlen(tok);
        return i + tl <= n && buf.compare(i, tl, tok) == 0;
    };

    size_t i = 0;
    while (i < n) {
        char c = buf[i];
        switch (st) {
        case St::Code:
            if (cfg.cpp_raw_strings && starts(i, "R\"")) {
                size_t opener = i + 2;
                while (opener < n && opener - (i + 2) <= 16 &&
                       buf[opener] != '(' && buf[opener] != ')' && buf[opener] != '\\' &&
                       buf[opener] != ' ' && buf[opener] != '\t' && buf[opener] != '\r' && buf[opener] != '\n')
                    ++opener;
                if (opener < n && buf[opener] == '(' && opener - (i + 2) <= 16) {
                    const std::string close = ")" + std::string(buf.substr(i + 2, opener - i - 2)) + '"';
                    const size_t end = buf.find(close, opener + 1);
                    const size_t to = end == std::string_view::npos ? n : end + close.size();
                    spans_.push_back({i, to, LexRole::Str});
                    i = to;
                    continue;
                }
            }
            if (starts(i, cfg.block_open)) {
                st = St::BlockComment;
                span_start = i;
                block_depth = 1;
                i += std::strlen(cfg.block_open);
                continue;
            }
            if (starts(i, cfg.line_comment) &&
                (!cfg.comment_needs_boundary || i == 0 || buf[i - 1] == ' ' ||
                 buf[i - 1] == '\t' || buf[i - 1] == '\n')) {
                st = St::LineComment;
                span_start = i;
                i += std::strlen(cfg.line_comment);
                continue;
            }
            if (cfg.triple_quotes && (c == '"' || c == '\'') &&
                (starts(i, "\"\"\"") || starts(i, "'''"))) {
                st = St::Str;
                span_start = i;
                quote = c;
                raw = false;
                multiline = true;
                triple = true;
                i += 3;
                continue;
            }
            if (c != '\0' && std::strchr(cfg.ml_quotes, c) != nullptr) {
                st = St::Str;
                span_start = i;
                quote = c;
                raw = (c == '`');
                multiline = true;
                triple = false;
                ++i;
                continue;
            }
            if (c != '\0' && std::strchr(cfg.sl_quotes, c) != nullptr) {
                st = St::Str;
                span_start = i;
                quote = c;
                raw = false;
                multiline = false;
                triple = false;
                ++i;
                continue;
            }
            ++i;
            break;
        case St::LineComment:
            if (c == '\n') {
                spans_.push_back({span_start, i, LexRole::Comment});
                st = St::Code;
            }
            ++i;
            break;
        case St::BlockComment:
            if (cfg.nest_blocks && starts(i, cfg.block_open)) {
                ++block_depth;
                i += std::strlen(cfg.block_open);
                continue;
            }
            if (starts(i, cfg.block_close)) {
                i += std::strlen(cfg.block_close);
                if (--block_depth == 0) {
                    spans_.push_back({span_start, i, LexRole::Comment});
                    st = St::Code;
                }
                continue;
            }
            ++i;
            break;
        case St::Str:
            if (!raw && c == '\\') {
                i += 2;
                continue;
            }
            if (!multiline && c == '\n') {
                // Unterminated single-line literal: close it at the newline
                // so one stray quote can't poison the rest of the file.
                spans_.push_back({span_start, i, LexRole::Str});
                st = St::Code;
                ++i;
                continue;
            }
            if (c == quote &&
                (!triple ||
                 starts(i, quote == '"' ? "\"\"\"" : "'''"))) {
                i += triple ? 3 : 1;
                spans_.push_back({span_start, i, LexRole::Str});
                st = St::Code;
                continue;
            }
            ++i;
            break;
        }
    }
    if (st != St::Code) {
        spans_.push_back({span_start, n,
                          st == St::Str ? LexRole::Str : LexRole::Comment});
    }

    if (!cfg.import_prefixes.empty()) {
        const uint32_t nlines = idx.line_count();
        for (uint32_t L = 1; L <= nlines; ++L) {
            std::string_view t = idx.line_text(L);
            if (t.data() == nullptr) continue;
            size_t ws = 0;
            while (ws < t.size() && (t[ws] == ' ' || t[ws] == '\t')) ++ws;
            if (ws >= t.size()) continue;
            uint64_t off = static_cast<uint64_t>(t.data() - buf.data()) + ws;
            if (at(off) != LexRole::Code) continue;
            std::string_view rest = t.substr(ws);
            for (const auto &pref : cfg.import_prefixes) {
                if (rest.size() >= pref.size() &&
                    rest.compare(0, pref.size(), pref) == 0) {
                    import_lines_.push_back(L);
                    break;
                }
            }
        }
    }
}

LexRole RoleIndex::at(uint64_t offset) const {
    auto it = std::upper_bound(spans_.begin(), spans_.end(), offset,
                               [](uint64_t off, const Span &s) {
                                   return off < s.from;
                               });
    if (it == spans_.begin()) return LexRole::Code;
    --it;
    return offset < it->to ? it->role : LexRole::Code;
}

bool RoleIndex::import_line(uint32_t line) const {
    return std::binary_search(import_lines_.begin(), import_lines_.end(),
                              line);
}

} // namespace hpr
