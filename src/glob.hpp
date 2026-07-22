// Path-aware glob matcher.
//
// Supports the documented subset:
//   *      one path segment, any chars except '/'
//   ?      one char except '/'
//   **     zero or more path segments
//   [abc]  / [a-z]  character class within one segment
//   {a,b}  alternation (may nest, may span '/'), expanded before matching
//
// Matching is path-aware: segments are split on '/'. ** is the only token
// that can cross segment boundaries.
#pragma once

#include <string>
#include <string_view>

namespace hpr {

bool has_glob_chars(std::string_view s);
bool glob_match(std::string_view pat, std::string_view path);

// Split a glob into a fixed base prefix (no glob chars) and the magic
// suffix. Used by the walker to start fs traversal at the deepest
// non-magic directory rather than always recursing from cwd.
struct GlobSplit {
    std::string base;   // "." when no fixed prefix exists
    std::string suffix; // glob relative to base; "" if pattern is literal
};
GlobSplit split_glob(std::string_view pat);

} // namespace hpr
