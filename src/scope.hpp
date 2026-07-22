// Enclosing-scope annotation: per-match identification of the nearest
// containing function/class/struct.
//
// Approach: a small Vectorscan + std::regex anchor pre-pass identifies each
// scope's signature, then `find_balanced_block` walks forward to find the
// body's closing delimiter. Each match's $LINE/$FROM is then binary-searched
// against this sorted set of ranges to find the innermost containing scope.
//
// The scope detector is deliberately language-light: built-in packs cover
// brace-delimited languages (Go, Rust, C/C++, Java, JS, TS) with reasonably
// targeted regexes. Anything more exotic (Python indentation, Ruby end
// keywords, template-heavy C++) should use the explicit
// `-scope-pattern`/`-scope-open`/`-scope-close` escape hatch.
#pragma once

#include "line_index.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct ScopeRange {
    uint64_t start_off = 0;   // byte offset of signature start (inclusive)
    uint64_t end_off = 0;     // byte offset of body's closing delim (exclusive)
    std::string name;
    std::string kind;         // "func", "class", "struct", …
    uint32_t line_start = 0;  // 1-based, points at the signature line
    uint32_t line_end = 0;    // 1-based, points at the closing-brace line
};

struct ScopeConfig {
    std::string anchor_regex; // PCRE; first capture group is the scope name
    std::string open;         // body opening delimiter
    std::string close;        // body closing delimiter
    std::string kind;         // emitted in records as `enclosing.kind`
    // Names the anchor regex can match but that are NOT scopes. The C-family
    // packs need this: `name(...) {` also fits `if (...) {` / `for (...) {`,
    // and Hyperscan has no lookahead to exclude keywords in the regex itself.
    std::vector<std::string> skip_names;
};

class ScopeIndex {
public:
    // Build a sorted list of scope ranges by anchor-scanning `buf`. Returns
    // false if the anchor regex fails to compile (Vectorscan or std::regex
    // side); err is populated. Empty result is *not* an error.
    bool build(std::string_view buf, const ScopeConfig &cfg,
               const LineIndex &idx, std::string *err);

    bool empty() const { return ranges_.empty(); }

    // Innermost scope strictly containing `offset`, or nullptr. The deepest
    // (smallest) containing range wins.
    const ScopeRange *find_innermost(uint64_t offset) const;

private:
    // Sorted by start_off ascending; nested ranges are interleaved.
    std::vector<ScopeRange> ranges_;
};

// Look up a built-in language pack by name (`"go"`, `"rust"`, `"c"`, `"cpp"`,
// `"java"`, `"js"`, `"ts"`). Returns nullptr when unknown. `"auto"` is *not*
// handled here — call `auto_lang_for_path` and feed the result back in.
const ScopeConfig *builtin_scope_pack(const std::string &lang);

// Resolve `auto` mode: returns a built-in pack name based on the file's
// extension, or "" if no pack maps to it.
std::string auto_lang_for_path(const std::string &path);

// Convenience: take the user-provided scope_lang ("auto", "go", …, or "") and
// custom (`pattern`/`open`/`close`/`kind` from CLI/script) plus the file path,
// and return a ScopeConfig (empty pattern → no scope detection for this file).
ScopeConfig resolve_scope_for_file(const std::string &lang,
                                   const ScopeConfig &custom,
                                   const std::string &path);

} // namespace hpr
