#include "matcher.hpp"

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
        return 1; // non-zero asks Hyperscan to stop scanning.
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

    // Hyperscan needs raw arrays. Build them; keep the wrapped strings alive
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
                                                     : "hyperscan compile failed";
            err->pattern_index = hs_err ? hs_err->expression : -1;
        }
        if (hs_err) hs_free_compile_error(hs_err);
        return false;
    }

    rc = hs_alloc_scratch(db_, &scratch_);
    if (rc != HS_SUCCESS) {
        if (err) err->message = "hs_alloc_scratch failed";
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
    // HS_INVALID can occur in UTF-8 mode if the input contains invalid
    // UTF-8 sequences — we treat that as "scan finished with whatever
    // matches we've already seen" rather than a hard failure, so the
    // caller can still emit partial results from the file.
    return rc == HS_SUCCESS || rc == HS_SCAN_TERMINATED || rc == HS_INVALID;
}

} // namespace hpr
