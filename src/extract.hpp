// Capture-group extraction (`extract_names` on a Pattern).
//
// Hyperscan ignores `(...)` groups, so we run the same regex with std::regex
// (ECMAScript flavor) over each match's $MATCH text to pull groups out. The
// table is built once after compilation and consulted on every emission.
#pragma once

#include "common.hpp"

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct ExtractEntry {
    std::regex regex;
    std::vector<std::string> names; // names[i] -> capture group i+1
};

class ExtractTable {
public:
    // Build a table from `patterns`. Returns true on success; on failure,
    // `*err` is set and `*err_pat_idx` (when non-null) names the offending
    // pattern. Patterns with empty extract_names produce no entry.
    bool build(const std::vector<Pattern> &patterns, std::string *err,
               int *err_pat_idx);

    // Returns true if any pattern in this table has extract entries.
    bool any() const { return any_; }

    // Returns true if pattern_index has extract entries.
    bool has(uint32_t pattern_index) const {
        return pattern_index < entries_.size() &&
               !entries_[pattern_index].names.empty();
    }

    // Names list for a pattern (empty when no entry).
    const std::vector<std::string> &names(uint32_t pattern_index) const {
        return entries_[pattern_index].names;
    }

    // Run the extraction over `match_text`; values[i] corresponds to
    // names()[i]. Missing/empty groups are returned as empty strings.
    void extract(uint32_t pattern_index, std::string_view match_text,
                 std::vector<std::string> &values) const;

private:
    std::vector<ExtractEntry> entries_; // indexed by pattern_index
    bool any_ = false;
};

} // namespace hpr
