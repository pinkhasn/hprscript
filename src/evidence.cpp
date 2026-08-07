#include "evidence.hpp"

#include <algorithm>

namespace hpr {

void accumulate_rank_input(RankInput &in, const std::string &file,
                           const std::vector<Match> &matches,
                           const std::vector<Pattern> &patterns,
                           const LineIndex &idx) {
    auto &fr = in.per_file[file];
    if (fr.matched_pat_ids.empty() && fr.match_points.empty())
        in.file_order.push_back(file);
    fr.line_count = idx.line_count();
    static constexpr size_t kMaxMatchPoints = 4096;
    for (const auto &m : matches) {
        const Pattern &pat = patterns[m.pattern_index];
        if (fr.matched_pat_ids.insert(pat.id).second)
            fr.raw_score += pat.weight;
        if (fr.match_points.size() >= kMaxMatchPoints) continue;
        auto [it, inserted] = fr.pat_local_ids.emplace(
            pat.id, static_cast<uint16_t>(std::min<size_t>(
                        fr.pat_local_ids.size(), 0xFFFFu)));
        fr.match_points.emplace_back(idx.line_of(m.from), it->second);
    }
}

} // namespace hpr
