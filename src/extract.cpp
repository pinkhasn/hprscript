#include "extract.hpp"

namespace hpr {

bool ExtractTable::build(const std::vector<Pattern> &patterns,
                         std::string *err, int *err_pat_idx) {
    entries_.assign(patterns.size(), ExtractEntry{});
    any_ = false;
    for (size_t i = 0; i < patterns.size(); ++i) {
        const Pattern &p = patterns[i];
        if (p.extract_names.empty()) continue;
        auto flags = std::regex::ECMAScript;
        if (p.case_insensitive) flags |= std::regex::icase;
        try {
            entries_[i].regex = std::regex(p.regexp, flags);
            entries_[i].names = p.extract_names;
            any_ = true;
        } catch (const std::regex_error &e) {
            if (err) {
                *err = std::string("extract regex incompatible with std::regex: ")
                       + e.what() + " (use submatch instead)";
            }
            if (err_pat_idx) *err_pat_idx = static_cast<int>(i);
            return false;
        }
    }
    return true;
}

void ExtractTable::extract(uint32_t pattern_index, std::string_view match_text,
                           std::vector<std::string> &values) const {
    values.clear();
    if (pattern_index >= entries_.size()) return;
    const ExtractEntry &e = entries_[pattern_index];
    if (e.names.empty()) return;
    values.resize(e.names.size());

    std::cmatch m;
    if (std::regex_search(match_text.data(),
                          match_text.data() + match_text.size(), m, e.regex)) {
        for (size_t i = 0; i < e.names.size(); ++i) {
            // m[0] is the whole match; capture groups start at 1.
            size_t g = i + 1;
            if (g < m.size() && m[g].matched) {
                values[i].assign(m[g].first, m[g].second);
            }
        }
    }
}

} // namespace hpr
