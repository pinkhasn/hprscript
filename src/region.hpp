// Pattern-defined regions: spans of a file delimited by matches of an
// "open" regex and (optionally) a "close" regex, declared on the command
// line with -region/-region-open/-region-close. They generalise enclosing
// scopes to any text — markdown sections, log sessions, #ifdef blocks, SQL
// transactions, code fences — without a language pack.
//
// Three delimiting modes, chosen per definition:
//   * section (no close)   each open match starts a region that runs to the
//                          next open match, or to end of file.
//   * range (open + close) awk/sed range semantics: the region runs from an
//                          open match through the first close match that
//                          starts at or after the open match's end; open
//                          matches inside it are ignored. With no close
//                          match it runs to end of file (closed = false).
//   * nested (-region-nested) balanced open/close with a stack; regions of
//                          one definition may contain each other.
//
// The delimiters are compiled once per run into their own Vectorscan
// database (RegionSet) and scanned per file into a RegionIndex — the same
// shape as ScopeIndex, so match annotation, -in-region filters and
// -same-region relations are lookups against sorted byte ranges.
#pragma once

#include "common.hpp"
#include "line_index.hpp"
#include "matcher.hpp"

#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct RegionDef {
    std::string id;     // -region <id>; identifier-shaped
    std::string open;   // PCRE; capture group 1 (when present) = label
    std::string close;  // PCRE; empty → section mode
    bool nested = false;
};

struct RegionRange {
    uint32_t def = 0;        // index into RegionSet::defs()
    uint64_t start_off = 0;  // start of the opening match (inclusive)
    uint64_t end_off = 0;    // end of the region (exclusive)
    uint32_t line_start = 0; // 1-based
    uint32_t line_end = 0;   // 1-based, line of the region's last byte
    uint32_t depth = 1;      // 1 = outermost; > 1 only for nested defs
    int32_t parent = -1;     // index into RegionIndex::all(); nested defs only
    // False when a range/nested region found no closing match and ran to
    // end of file. Section regions are always closed (the next open match
    // or end of file is their defined end).
    bool closed = true;
    std::string label;       // capture group 1 of the opening match, or its
                             // first line when the regex has no group
};

// The run-constant half: definitions plus their compiled delimiters.
class RegionSet {
public:
    // Compile every definition's delimiters. `utf8` mirrors the search
    // patterns' mode (-no-utf8 → false). Returns false with *err set when a
    // delimiter fails to compile.
    bool init(const std::vector<RegionDef> &defs, bool utf8, std::string *err);

    bool active() const { return !defs_.empty(); }
    const std::vector<RegionDef> &defs() const { return defs_; }
    size_t pattern_count() const { return patterns_.size(); }

    // Index of the definition named `id`, or -1.
    int find(const std::string &id) const;

private:
    friend class RegionIndex;

    // Label for a region whose opening match text is `open_text`.
    std::string label_for(uint32_t def, std::string_view open_text) const;

    struct Slot {
        uint32_t def = 0;
        bool is_close = false;
    };
    std::vector<RegionDef> defs_;
    std::vector<Pattern> patterns_; // delimiter patterns, in slot order
    std::vector<Slot> slots_;       // pattern index → definition/side
    Matcher matcher_;
    std::vector<std::regex> label_re_;
    std::vector<char> has_label_re_;
};

// The per-file half: every region found in one buffer.
class RegionIndex {
public:
    // Scan `buf` with the set's delimiters and build the range list. `set`
    // is borrowed and must outlive the index (it is not const because a
    // Vectorscan scan mutates the matcher's scratch space).
    void build(std::string_view buf, RegionSet &set, const LineIndex &idx);

    const RegionSet *set() const { return set_; }
    bool empty() const { return ranges_.empty(); }

    // Every region, grouped by definition (definition order), each group
    // sorted by start_off ascending. Nested regions interleave with their
    // children in start order.
    const std::vector<RegionRange> &all() const { return ranges_; }

    // The same regions in file order (start_off, then definition order) —
    // the order -list-regions prints.
    std::vector<const RegionRange *> in_file_order() const;

    // Innermost region of definition `def` containing `offset`, or nullptr.
    const RegionRange *find_innermost(uint32_t def, uint64_t offset) const;

    // Enclosing region of `r` within the same definition (nested defs), or
    // nullptr.
    const RegionRange *parent_of(const RegionRange &r) const {
        return r.parent < 0 ? nullptr : &ranges_[static_cast<size_t>(r.parent)];
    }

private:
    const RegionSet *set_ = nullptr;
    std::vector<RegionRange> ranges_;
    std::vector<size_t> def_begin_; // ranges_ offsets per def, size defs+1
};

} // namespace hpr
