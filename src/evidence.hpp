#pragma once

#include "common.hpp"
#include "line_index.hpp"
#include "rank.hpp"

#include <string>
#include <vector>

namespace hpr {

void accumulate_rank_input(RankInput &in, const std::string &file,
                           const std::vector<Match> &matches,
                           const std::vector<Pattern> &patterns,
                           const LineIndex &idx);

} // namespace hpr
