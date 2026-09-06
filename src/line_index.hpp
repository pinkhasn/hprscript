// Maps byte offsets in a buffer to 1-based line/column coordinates.
//
// Built once per file by scanning for '\n'. Lookups are O(log n) via
// binary search over the newline offsets, and the line text + context
// lines can be sliced cheaply from the original buffer.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace hpr {

class LineIndex {
public:
    LineIndex() = default;
    void build(std::string_view data);

    // 1-based line number for byte offset (offset must be in [0, size]).
    uint32_t line_of(uint64_t offset) const;
    // 1-based column for byte offset within its line.
    uint32_t col_of(uint64_t offset) const;

    // Inclusive byte range [start,end) of the line containing offset.
    // end excludes the terminating '\n' (if any) so callers can slice
    // the line text without trimming themselves.
    void line_range(uint64_t offset, uint64_t &start, uint64_t &end) const;

    // Slice text for a line by 1-based line number (1..line_count()).
    std::string_view line_text(uint32_t line_no) const;

    uint32_t line_count() const { return line_count_; }
    uint64_t memory_bytes() const { return sizeof(*this) + newlines_.capacity() * sizeof(uint64_t); }

private:
    std::string_view data_;
    // Sorted byte offsets of every '\n' in data_. Empty for files with no
    // newlines (treated as a single line).
    std::vector<uint64_t> newlines_;
    uint32_t line_count_ = 1;
};

} // namespace hpr
