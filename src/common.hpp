// Shared types used across hprscript components.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hpr {

// One pattern in a multi-pattern compile. id is the index into the user's
// pattern list and is what Vectorscan reports back on a match.
struct Pattern {
    std::string id;        // user-facing identifier (defaults to "p<idx>")
    std::string regexp;    // raw PCRE regex passed to Vectorscan
    bool case_insensitive = false;
    bool word_boundary = false; // wrap as \b(?:expr)\b before compile
    // UTF-8 mode (on by default): `.` matches one codepoint, case-insensitive
    // folds across scripts (CAFÉ↔café, ПРИВЕТ↔привет). Disable with
    // `-no-utf8` for byte-mode on non-UTF-8 input.
    bool utf8 = true;
    // UCP (off by default): Unicode property categories for `\w`/`\d`/`\s`.
    // Off by default because Vectorscan rejects many UCP+`\w+`-style patterns
    // as "too large". Opt in with `-ucp` when you really need it.
    bool ucp = false;
    double weight = 1.0;        // reserved for ranking mode (post-MVP)

    // Free-text meaning of the pattern (-desc / rule-file `description`).
    // Surfaced once as a query-legend line in -llm/-elide output.
    std::string desc;

    // Reference-only pattern (edit mode's -ref): participates in matching —
    // relations and -file-where see its matches — but never produces edit
    // sites. The qualifier in `-far hit:allow:0` needs this, or its own
    // matches would be rewritten too.
    bool ref = false;

    // When non-empty, capture groups in the regex are re-extracted via a
    // std::regex (ECMAScript) post-pass over $MATCH and surfaced under these
    // names — names[i] maps to capture group i+1. Compile failures are
    // reported at the same point as Vectorscan compile failures.
    std::vector<std::string> extract_names;
};

// One reported match from a scan over a file/buffer.
struct Match {
    uint32_t pattern_index = 0; // index into Pattern[] array
    uint64_t from = 0;          // byte offset of match start (inclusive)
    uint64_t to = 0;            // byte offset of match end (exclusive)
};

} // namespace hpr
