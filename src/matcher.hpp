// Thin wrapper over Hyperscan's hs_compile_multi + hs_scan.
//
// Compiles a vector of Patterns into a single block-mode database with
// HS_FLAG_SOM_LEFTMOST (so we get accurate match start offsets) plus
// per-pattern HS_FLAG_CASELESS when requested. Word-boundary patterns
// are handled by wrapping the regex as \b(?:...)\b before compile.
//
// Hyperscan reports overlapping/repeated matches eagerly. Most grep-like
// tools want one hit per (pattern, position) so callers can dedupe; for
// the MVP we report every match the engine emits and let the output
// layer collapse as needed.
#pragma once

#include "common.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <hs/hs.h>

namespace hpr {

struct CompileError {
    std::string message;     // human-readable error from Hyperscan
    int pattern_index = -1;  // -1 if not attributable to a single pattern
};

class Matcher {
public:
    Matcher() = default;
    ~Matcher();
    Matcher(const Matcher &) = delete;
    Matcher &operator=(const Matcher &) = delete;

    // Compile patterns into a single database. Returns true on success;
    // on failure, *err is populated. Patterns is borrowed (must outlive scans).
    bool compile(const std::vector<Pattern> &patterns, CompileError *err);

    // Scan a buffer and invoke `cb` for every match. Return false from cb
    // to stop scanning early (Hyperscan returns HS_SCAN_TERMINATED).
    using MatchCb = std::function<bool(const Match &)>;
    bool scan(std::string_view buf, const MatchCb &cb);

private:
    hs_database_t *db_ = nullptr;
    hs_scratch_t *scratch_ = nullptr;
};

} // namespace hpr
