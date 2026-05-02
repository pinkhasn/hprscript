#include "line_index.hpp"

#include <algorithm>
#include <climits>

namespace hpr {

void LineIndex::build(std::string_view data) {
    data_ = data;
    newlines_.clear();
    for (uint64_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\n') newlines_.push_back(i);
    }
    // line count = (#newlines) + 1, except an empty file is still 1 line.
    line_count_ = static_cast<uint32_t>(newlines_.size() + 1);
}

uint32_t LineIndex::line_of(uint64_t offset) const {
    // Number of newlines strictly before offset, +1 for 1-based line numbers.
    auto it = std::lower_bound(newlines_.begin(), newlines_.end(), offset);
    return static_cast<uint32_t>((it - newlines_.begin()) + 1);
}

uint32_t LineIndex::col_of(uint64_t offset) const {
    uint64_t start, end;
    line_range(offset, start, end);
    uint64_t col = (offset - start) + 1;
    return col > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(col);
}

void LineIndex::line_range(uint64_t offset, uint64_t &start, uint64_t &end) const {
    auto it = std::lower_bound(newlines_.begin(), newlines_.end(), offset);
    // Start = char after the previous newline (or 0 if none).
    if (it == newlines_.begin()) {
        start = 0;
    } else {
        start = *(it - 1) + 1;
    }
    // End = position of next newline (or buffer end if none).
    if (it == newlines_.end()) {
        end = data_.size();
    } else {
        end = *it;
    }
}

std::string_view LineIndex::line_text(uint32_t line_no) const {
    if (line_no == 0 || line_no > line_count_) return {};
    uint64_t start = (line_no == 1) ? 0 : (newlines_[line_no - 2] + 1);
    uint64_t end = (line_no - 1 < newlines_.size()) ? newlines_[line_no - 1]
                                                    : data_.size();
    return data_.substr(start, end - start);
}

} // namespace hpr
