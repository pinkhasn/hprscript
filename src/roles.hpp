// Per-match lexical role classification: is a byte offset inside a comment
// or a string literal, and is its line an import-ish line?
//
// A single linear pass over the file builds comment/string spans with an
// escape-aware state machine (line comments, block comments with optional
// nesting, single-line and multiline string literals, Python triple quotes).
// Match offsets are then binary-searched against the span list. This is
// lexical classification, not parsing — but comment/string boundaries are
// exactly the distinctions a lexer gets right, and they're the ones a reader
// of search results otherwise has to re-derive per match.
//
// The fourth role, `def`, is not computed here: a match whose line is a
// scope-anchor line is classified by ScopeIndex::anchor_on_line (scope.hpp).
#pragma once

#include "line_index.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

enum class LexRole : uint8_t { Code, Comment, Str };

struct RoleConfig {
    const char *line_comment = nullptr; // "//" or "#" (nullptr = none)
    // Shell only: "#" starts a comment only at line start or after
    // whitespace ($#, ${#x} must not count). Python/YAML have no such rule.
    bool comment_needs_boundary = false;
    const char *block_open = nullptr;   // "/*" (nullptr = none)
    const char *block_close = nullptr;  // "*/"
    bool nest_blocks = false;           // Rust nests /* /* */ */
    const char *sl_quotes = "";         // single-line delims; close at newline
    const char *ml_quotes = "";         // multiline delims (Go/JS "`", Rust "\"")
    bool triple_quotes = false;         // Python """ / ''' (multiline)
    std::vector<std::string> import_prefixes; // "import ", "#include", …
};

// Language config for a path by extension, or nullptr when unknown. Covers
// the scope-pack languages (go, rust, c, cpp, java, js, ts) plus
// hash-comment ones (py, sh, rb, yaml, toml) that have no scope pack.
const RoleConfig *role_config_for_path(const std::string &path);

class RoleIndex {
public:
    // Single pass over `buf`; `idx` is only used for import-line marking.
    void build(std::string_view buf, const RoleConfig &cfg,
               const LineIndex &idx);

    // Role at a byte offset (Code when outside every span).
    LexRole at(uint64_t offset) const;

    // True when `line` (1-based) starts — after leading whitespace, in code —
    // with one of the language's import prefixes.
    bool import_line(uint32_t line) const;

private:
    struct Span {
        uint64_t from, to; // [from, to)
        LexRole role;      // Comment or Str only
    };
    std::vector<Span> spans_;          // sorted by `from`, non-overlapping
    std::vector<uint32_t> import_lines_; // sorted, 1-based
};

} // namespace hpr
