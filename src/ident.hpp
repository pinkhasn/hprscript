// Identifier-subtoken matching (`-ident`): find identifiers regardless of
// naming convention — `parseConfig` ~ `parse_config` ~ `ConfigParser` all
// contain the subtokens "parse" and "config". This is the recall gap plain
// regex search can't close without the caller enumerating every casing
// variant by hand.
//
// Deliberately ASCII-only ([A-Za-z_][A-Za-z0-9_]*) and hand-scanned rather
// than routed through Vectorscan: the term set is dynamic per invocation,
// so it doesn't fit the compiled-pattern-database model the rest of the
// engine uses. Non-ASCII identifiers (Unicode identifiers in Go, etc.) are
// silently not scanned.
#pragma once

#include "common.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

// One `-ident` group: terms that must ALL appear as subtokens of the same
// identifier (AND within a group). Separate `-ident` occurrences OR
// together — each becomes its own group/synthetic pattern. Terms are
// lowercased by the CLI parser; matching is always case-insensitive.
struct IdentGroup {
    std::vector<std::string> terms;
};

struct IdentifierToken {
    uint64_t from = 0;
    uint64_t to = 0;
};

// Shared ASCII identifier tokenizer. Investigation reuses this exact lexical
// definition when deriving related candidates, while -ident layers subtoken
// matching on top of the returned spans.
void scan_identifier_tokens(std::string_view buf,
                            std::vector<IdentifierToken> &out);

// Byte offsets into `ident` where a subtoken begins: after each `_`
// (consumed, never itself part of a token), at lower→upper transitions
// (camelCase), at alpha↔digit transitions, and — for acronym runs like
// "HTTPServer" — at the last uppercase letter before a following lowercase
// run, so the split reads "HTTP", "Server" rather than "HTTPS", "erver".
// Exposed for testing; scan_identifiers() is the entry point callers use.
std::vector<size_t> identifier_subtoken_starts(std::string_view ident);

// Scan `buf` for identifier runs, testing each against every group in
// `groups`. A group matches an identifier when every one of its terms
// appears (case-insensitively) as a substring starting exactly at one of
// the identifier's subtoken boundaries — so "utf8" matches "UTF8Decoder"
// (starts at the identifier's own start, spanning the internal digit
// boundary) but "arse" does not match "parseConfig" (no boundary mid-word).
// One Match per (identifier, matching group), appended in scan order;
// pattern_index = `pattern_index_base` + the group's index in `groups`.
void scan_identifiers(std::string_view buf, const std::vector<IdentGroup> &groups,
                      uint32_t pattern_index_base, std::vector<Match> &out);

} // namespace hpr
