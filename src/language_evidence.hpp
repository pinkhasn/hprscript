#pragma once

#include <string>
#include <string_view>
#include "roles.hpp"
#include "scope.hpp"

namespace hpr {

struct OccurrenceClassification {
    std::string classification = "unclassified_reference";
    std::string confidence = "low";
    std::string method = "lexical-generic-pack";
};

// Shared by investigation and the streaming formatter. File roles are
// independent of occurrence roles; lexical positions win over heuristics.
const char *occurrence_role(const RoleIndex *roles, const ScopeIndex *scope,
                           uint64_t from, uint64_t to, uint32_t line,
                           const ScopeRange **definition = nullptr);

OccurrenceClassification classify_occurrence(
    const std::string &path, std::string_view content, uint64_t from,
    uint64_t to, const LineIndex &lines, const ScopeIndex *scope,
    const RoleIndex *roles);

} // namespace hpr
