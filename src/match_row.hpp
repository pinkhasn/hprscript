#pragma once

#include "common.hpp"
#include "extract.hpp"
#include "line_index.hpp"
#include "scope.hpp"
#include "value.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct ScopeRef {
    std::string name;
    std::string kind;
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t line_start = 0;
    uint32_t line_end = 0;
};

// Shared owned representation used by investigation and query. Quick search
// keeps its streaming Match path and pays none of this materialization cost.
struct MatchRow {
    uint64_t row_id = 0;
    uint32_t set_index = 0;
    uint32_t pattern_index = 0;
    std::string set_id;
    std::string pattern_id;
    std::string file;
    std::string language;
    uint64_t from = 0;
    uint64_t to = 0;
    uint32_t line = 0;
    uint32_t column = 0;
    std::string match;
    std::string context;
    std::map<std::string, RuntimeValue> captures;
    std::optional<ScopeRef> enclosing;
    // Adaptive query sets populate these to preserve derived-pattern
    // provenance without losing typed source row identities.
    std::string derived_value;
    std::vector<uint64_t> derived_source_rows;
};

MatchRow materialize_match_row(uint64_t row_id, uint32_t set_index,
                               const std::string &set_id,
                               const std::vector<Pattern> &patterns,
                               const Match &match, const std::string &file,
                               std::string_view content, const LineIndex &idx,
                               const ScopeIndex *scope,
                               const ExtractTable *extract = nullptr);

// Field access used by the typed query evaluator. `field` is relative to a
// row (for example capture.name or enclosing.from).
RuntimeValue match_row_field(const MatchRow &row, const std::string &field,
                             bool *known = nullptr);

} // namespace hpr
