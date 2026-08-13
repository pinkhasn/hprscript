// `hprscript expand <file:line[@hash]>` — the drill-down half of the
// search → expand loop.
//
// Search output already names every hit as file:line; expand turns that
// pointer into the full enclosing scope without the caller reconstructing a
// pattern and re-scanning. The optional `@hash` (printed by search's -refs
// flag) is a content check on the ref's line: if the file changed
// underneath, expand either recovers the line at its new position (unique
// hash match elsewhere → "ref line moved") or reports the ref as stale
// instead of silently expanding the wrong code.
#pragma once

#include <string>
#include <string_view>

namespace hpr {

struct Cli;

// 6-hex-char content hash of a line's text, whitespace-trimmed so pure
// re-indentation doesn't invalidate refs. The single definition of ref
// identity — search's -refs output and expand's verification both use it.
std::string ref_hash6(std::string_view line_text);

// Exit codes: 0 = every ref expanded; 3 = at least one ref was stale
// (reported in-band on stdout); 2 = usage/read errors (stderr).
int run_expand(const Cli &cli);

} // namespace hpr
