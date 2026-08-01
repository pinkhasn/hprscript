// File-relevance scoring shared by script-mode `rank` and quick-search
// `-hotspots`. Extracted from script.cpp so both can call the identical
// formula instead of maintaining two implementations.
//
// The formula combines three signals: coverage (fraction of queried pattern
// ids the file matches, exponentiated so matching everything dominates),
// weighted hits (Σ weight over distinct matched pattern ids, normalized by
// file size), and a proximity bonus (matches from ≥2 distinct patterns
// within a small line window — "matches that live together in one
// function"). See HPRSCRIPT.md's "Match ranking" section for the full
// derivation and the `rank_surprise` / `rank_rich_clusters` opt-ins.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace hpr {

// Per-file accumulator fed incrementally while scanning.
struct FileRank {
    std::set<std::string> matched_pat_ids;
    double raw_score = 0.0; // Σ weight of distinct matched pattern IDs.
    uint32_t line_count = 1;
    // Per-file pattern-id → local index (0..N-1) for compact match_points.
    std::map<std::string, uint16_t> pat_local_ids;
    // Every match recorded as (1-based line, pat_local_id) for proximity
    // sweep and best-window selection.
    std::vector<std::pair<uint32_t, uint16_t>> match_points;
};

// Inputs the scoring formula needs, decoupled from any one caller's state.
struct RankInput {
    std::vector<std::string> file_order; // first-seen order (stable output)
    std::map<std::string, FileRank> per_file;
    std::set<std::string> queried_ids;   // non-absent pattern ids in the query
    uint32_t total_queried = 0;          // coverage denominator
    std::map<std::string, double> pattern_weights; // id -> weight
    // Opt-in: fold a corpus-derived surprise factor (IDF-style) into the
    // per-pattern weight — a pattern matching almost every file barely
    // distinguishes them; a rare pattern is boosted.
    bool surprise = false;
    // Opt-in: scale the proximity bonus by (distinct_pat_ids_in_cluster - 1)
    // instead of a flat contribution per qualifying cluster.
    bool rich_clusters = false;
};

struct RankRow {
    std::string file;
    double score = 0.0;
    double density = 0.0; // Σweight / line_count — diagnostic + tiebreaker
    std::vector<std::string> matched_patterns;
    // Populated only when RankInput::surprise is set: per-pattern factor.
    std::vector<std::pair<std::string, double>> surprise;
    // The file's single densest match cluster (most distinct pattern ids,
    // ties broken by point count), 1-based inclusive line range. {0,0} when
    // the file has no recorded match points.
    uint32_t window_lo = 0;
    uint32_t window_hi = 0;
};

// Count proximity clusters: contiguous groups of match points where each
// adjacent pair is within K=20 lines (roughly "same function" in typical
// code) and the group spans ≥2 distinct pattern IDs. `rich` scales each
// cluster's contribution by (distinct_ids - 1) instead of a flat 1.
uint32_t count_prox_clusters(std::vector<std::pair<uint32_t, uint16_t>> pts,
                             bool rich = false);

// Score every file in `in`, sorted score descending (density is the
// tiebreaker) — same order `flush_rank` prints in script mode.
std::vector<RankRow> rank_files(const RankInput &in);

} // namespace hpr
