#include "matcher.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace hpr {

namespace {
struct ScanCtx {
    const Matcher::MatchCb *cb;
    bool stopped;
};

int hs_event_handler(unsigned int id, unsigned long long from,
                     unsigned long long to, unsigned int /*flags*/,
                     void *ctx_void) {
    auto *ctx = static_cast<ScanCtx *>(ctx_void);
    Match m;
    m.pattern_index = id;
    m.from = from;
    m.to = to;
    if (!(*ctx->cb)(m)) {
        ctx->stopped = true;
        return 1; // non-zero asks Vectorscan to stop scanning.
    }
    return 0;
}
} // namespace

Matcher::~Matcher() {
    if (scratch_) hs_free_scratch(scratch_);
    if (db_) hs_free_database(db_);
}

bool Matcher::compile(const std::vector<Pattern> &patterns, CompileError *err) {
    if (patterns.empty()) {
        if (err) err->message = "no patterns to compile";
        return false;
    }

    // Vectorscan needs raw arrays. Build them; keep the wrapped strings alive
    // until after hs_compile_multi returns.
    std::vector<std::string> wrapped;
    wrapped.reserve(patterns.size());
    std::vector<const char *> exprs;
    std::vector<unsigned int> flags;
    std::vector<unsigned int> ids;
    exprs.reserve(patterns.size());
    flags.reserve(patterns.size());
    ids.reserve(patterns.size());

    for (size_t i = 0; i < patterns.size(); ++i) {
        const Pattern &p = patterns[i];
        std::string expr = p.regexp;
        if (p.word_boundary) expr = "\\b(?:" + expr + ")\\b";
        wrapped.push_back(std::move(expr));

        // Default flags: SOM_LEFTMOST so we get accurate match start, and
        // MULTILINE so ^/$ anchor on line boundaries (matching grep's
        // behaviour and what users typically expect).
        unsigned int f = HS_FLAG_SOM_LEFTMOST | HS_FLAG_MULTILINE;
        if (p.case_insensitive) f |= HS_FLAG_CASELESS;
        // UTF-8 mode: `.` matches one codepoint; non-ASCII patterns are
        // valid; UCP makes \w/\d/\s/case-folding Unicode-aware.
        if (p.utf8) f |= HS_FLAG_UTF8;
        if (p.utf8 && p.ucp) f |= HS_FLAG_UCP;
        flags.push_back(f);
        ids.push_back(static_cast<unsigned int>(i));
        exprs.push_back(wrapped.back().c_str());
    }

    hs_compile_error_t *hs_err = nullptr;
    hs_error_t rc = hs_compile_multi(exprs.data(), flags.data(), ids.data(),
                                     static_cast<unsigned int>(patterns.size()),
                                     HS_MODE_BLOCK, nullptr, &db_, &hs_err);
    if (rc != HS_SUCCESS) {
        if (err) {
            err->message = hs_err && hs_err->message ? hs_err->message
                                                     : "vectorscan compile failed";
            err->pattern_index = hs_err ? hs_err->expression : -1;
        }
        if (hs_err) hs_free_compile_error(hs_err);
        return false;
    }

    rc = hs_alloc_scratch(db_, &scratch_);
    if (rc != HS_SUCCESS) {
        if (err) err->message = "hs_alloc_scratch failed";
        // Don't rely on the destructor: callers may discard the Matcher
        // before it runs, and we want compile() to leave the object in a
        // clean "uncompiled" state on failure.
        hs_free_database(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

bool Matcher::scan(std::string_view buf, const MatchCb &cb) {
    if (!db_ || !scratch_) return false;
    ScanCtx ctx{&cb, false};
    hs_error_t rc = hs_scan(db_, buf.data(), buf.size(), 0, scratch_,
                            hs_event_handler, &ctx);
    // HS_SCAN_TERMINATED is expected when the callback asks to stop.
    // HS_INVALID can occur in UTF-8 mode when the input contains invalid
    // UTF-8 — but the engine makes no promise either way: it may also keep
    // matching straight past bad bytes (observed with ASCII patterns) or
    // stop early. Either outcome is treated as "scan finished with whatever
    // matches were reported", never a hard failure, so results on invalid
    // UTF-8 input are best-effort. Callers wanting defined semantics on
    // such data should use utf8=false (byte-level matching).
    return rc == HS_SUCCESS || rc == HS_SCAN_TERMINATED || rc == HS_INVALID;
}

void dedup_leftmost_longest(std::vector<Match> &raw, std::vector<Match> &kept) {
    // Single sort-based pass: by (pattern, from, -to). Within a pattern,
    // after this sort the longest match at each `from` comes first, and any
    // later match whose `from` lies before the previous kept match's `to`
    // is overlapping (skip).
    std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
        if (a.pattern_index != b.pattern_index)
            return a.pattern_index < b.pattern_index;
        if (a.from != b.from) return a.from < b.from;
        return a.to > b.to;
    });
    kept.clear();
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
}

} // namespace hpr
