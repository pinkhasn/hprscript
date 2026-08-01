#include "rank.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace hpr {

namespace {

struct Cluster {
    uint32_t lo, hi;
    std::set<uint16_t> ids;
    size_t count = 0;
};

// Sort by line and sweep: a new cluster starts whenever the gap to the
// previous point exceeds K lines. Shared by count_prox_clusters and the
// best-window search so both agree on what "one cluster" means.
std::vector<Cluster> build_clusters(std::vector<std::pair<uint32_t, uint16_t>> pts,
                                    uint32_t K) {
    std::vector<Cluster> clusters;
    if (pts.empty()) return clusters;
    std::sort(pts.begin(), pts.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    size_t i = 0;
    while (i < pts.size()) {
        size_t j = i + 1;
        while (j < pts.size() && pts[j].first - pts[j - 1].first <= K) ++j;
        Cluster c;
        c.lo = pts[i].first;
        c.hi = pts[j - 1].first;
        c.count = j - i;
        for (size_t k = i; k < j; ++k) c.ids.insert(pts[k].second);
        clusters.push_back(std::move(c));
        i = j;
    }
    return clusters;
}

// The single most "interesting" cluster: most distinct pattern ids first
// (that's what the proximity bonus itself rewards), then most points, then
// earliest — deterministic tiebreaking for stable output.
std::pair<uint32_t, uint32_t> best_window(
    const std::vector<std::pair<uint32_t, uint16_t>> &pts, uint32_t K) {
    if (pts.empty()) return {0, 0};
    std::vector<Cluster> clusters = build_clusters(pts, K);
    const Cluster *best = &clusters.front();
    for (const auto &c : clusters) {
        if (c.ids.size() != best->ids.size()) {
            if (c.ids.size() > best->ids.size()) best = &c;
        } else if (c.count > best->count) {
            best = &c;
        }
    }
    return {best->lo, best->hi};
}

} // namespace

uint32_t count_prox_clusters(std::vector<std::pair<uint32_t, uint16_t>> pts,
                             bool rich) {
    static constexpr uint32_t K = 20;
    if (pts.size() < 2) return 0;
    uint32_t total = 0;
    for (const auto &c : build_clusters(std::move(pts), K)) {
        if (c.ids.size() >= 2)
            total += rich ? static_cast<uint32_t>(c.ids.size() - 1) : 1u;
    }
    return total;
}

std::vector<RankRow> rank_files(const RankInput &in) {
    static constexpr uint32_t K = 20;
    static constexpr double kCoverageExp = 1.5;
    static constexpr double kProximityWeight = 0.5;
    const double queried = in.total_queried > 0
                               ? static_cast<double>(in.total_queried)
                               : 1.0;

    // Corpus-surprise weighting: surprise_p = log((N+1)/(df_p+1)) + 1. Below
    // 3 files the corpus is too small for document-frequency to mean
    // anything, so every factor collapses to 1 (equivalent to being off).
    std::map<std::string, double> surprise_p;
    const bool surprise_active = in.surprise && in.file_order.size() >= 3;
    if (in.surprise) {
        const double N = static_cast<double>(in.file_order.size());
        std::map<std::string, uint32_t> df;
        for (const auto &id : in.queried_ids) df[id] = 0;
        for (const auto &f : in.file_order) {
            auto fit = in.per_file.find(f);
            if (fit == in.per_file.end()) continue;
            for (const auto &id : fit->second.matched_pat_ids) {
                auto it = df.find(id);
                if (it != df.end()) ++it->second;
            }
        }
        for (const auto &id : in.queried_ids) {
            double s = 1.0;
            if (surprise_active) {
                double dfp = static_cast<double>(df[id]);
                s = std::log((N + 1.0) / (dfp + 1.0)) + 1.0;
                if (s < 1.0) s = 1.0;
            }
            surprise_p[id] = s;
        }
    }

    auto effective_weight = [&](const std::string &id) -> double {
        double base = 1.0;
        if (auto it = in.pattern_weights.find(id);
            it != in.pattern_weights.end()) base = it->second;
        if (in.surprise) {
            auto sit = surprise_p.find(id);
            if (sit != surprise_p.end()) base *= sit->second;
        }
        return base;
    };

    std::vector<RankRow> rows;
    rows.reserve(in.file_order.size());
    for (const auto &f : in.file_order) {
        auto fit = in.per_file.find(f);
        if (fit == in.per_file.end()) continue;
        const FileRank &fr = fit->second;

        double sum_weight = 0.0;
        for (const auto &id : fr.matched_pat_ids) sum_weight += effective_weight(id);
        double matched = static_cast<double>(fr.matched_pat_ids.size());
        double coverage = matched / queried;
        if (coverage > 1.0) coverage = 1.0;
        double cov_factor = std::pow(coverage, kCoverageExp);
        double divisor = std::log(static_cast<double>(fr.line_count) + 10.0);
        if (divisor <= 0.0) divisor = 1.0;
        uint32_t clusters = count_prox_clusters(fr.match_points, in.rich_clusters);
        double prox_bonus = kProximityWeight * static_cast<double>(clusters);

        RankRow r;
        r.file = f;
        r.score = cov_factor * sum_weight / divisor + prox_bonus;
        r.density = fr.line_count > 0 ? sum_weight / static_cast<double>(fr.line_count) : 0.0;
        r.matched_patterns.assign(fr.matched_pat_ids.begin(), fr.matched_pat_ids.end());
        if (in.surprise) {
            r.surprise.reserve(fr.matched_pat_ids.size());
            for (const auto &id : fr.matched_pat_ids) {
                auto sit = surprise_p.find(id);
                r.surprise.emplace_back(id, sit != surprise_p.end() ? sit->second : 1.0);
            }
        }
        std::tie(r.window_lo, r.window_hi) = best_window(fr.match_points, K);
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const RankRow &a, const RankRow &b) {
        if (a.score != b.score) return a.score > b.score;
        return a.density > b.density;
    });
    return rows;
}

} // namespace hpr
