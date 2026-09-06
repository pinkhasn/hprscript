#pragma once

#include "file_role.hpp"
#include "language_evidence.hpp"
#include "match_row.hpp"
#include "output.hpp"
#include "rank.hpp"
#include "source_chunk.hpp"

#include <array>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace hpr {

enum EvidenceCategory { SeedDefinition, Dependency, Caller, Test, Config, Documentation, Other, CategoryCount };
const char *category_name(EvidenceCategory category);

struct EvidenceOrigin {
    uint64_t source_row = 0;
    uint32_t seed = 0;
    std::string file;
    uint32_t line = 0;
    uint64_t scope_from = 0;
    std::string kind;
    int priority = 3;
};

struct InvestigationCandidate {
    std::string identifier;
    std::vector<EvidenceOrigin> origins;
    uint64_t same_scope_hits = 0, same_window_hits = 0, same_file_hits = 0;
    uint64_t seed_files = 0, corpus_files = 0, definitions = 0;
    int priority = 3;
    double score = 0;
};

struct InvestigationEvidence {
    MatchRow row;
    OccurrenceClassification classification;
    std::vector<std::string> file_roles;
    std::vector<EvidenceOrigin> origins;
    EvidenceCategory category = Other;
    std::string chunk_key;
    std::string ref;
    bool derived = false;
    bool ambiguous = false;
};

// Explicit accounting of retained evidence, not total RSS. File mappings,
// matcher scratch, transient per-file indexes, and serialization are excluded.
struct EvidenceMemory {
    uint64_t limit = 0, used = 0, peak = 0;
    bool reserve(uint64_t bytes) {
        if (limit && (used > limit || bytes > limit - used)) return false;
        used += bytes;
        peak = std::max(peak, used);
        return true;
    }
    void release(uint64_t bytes) { used -= std::min(bytes, used); }
    uint64_t remaining() const { return limit ? limit - std::min(limit, used) : 0; }
};

struct InvestigationReport {
    std::string profile;
    std::vector<std::string> seeds;
    ScanStats stats;
    std::vector<InvestigationEvidence> evidence;
    std::map<std::string, SourceChunk> chunks;
    std::vector<InvestigationCandidate> candidates;
    std::vector<RankRow> ranked_files;
    std::array<uint64_t, CategoryCount> found{};
    uint64_t seed_files = 0, seed_definitions = 0, seed_tests = 0, seed_configs = 0;
    uint64_t related_matches = 0, related_tests = 0, followup_failures = 0;
    uint64_t candidate_omissions = 0, retention_omissions = 0, memory_omissions = 0;
    uint64_t scope_fallbacks = 0, warning_omissions = 0;
    bool followup_ran = false, followup_requested = true;
    std::string plan_record;
    std::vector<std::string> warnings;
};

// One serialization/selection boundary for the complete stdout payload.
// Returns 2 without writing stdout if even the minimum report cannot fit.
int emit_investigation_report(const Cli &cli, InvestigationReport &report,
                              uint64_t elapsed_ms);

} // namespace hpr
