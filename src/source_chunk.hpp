#pragma once

#include "cli.hpp"
#include "line_index.hpp"
#include "scope.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace hpr {

// Owned exact source, independent of match rows and mapped-file lifetimes.
// Multiple occurrences can share a chunk without duplicating its body.
struct SourceLine {
    std::string text;
    uint64_t omitted_bytes = 0;
};

struct SourceChunk {
    std::string file;
    uint32_t first = 0, last = 0;
    uint32_t signature_first = 0, signature_last = 0;
    bool reliable_scope = false;
    bool retention_reduced = false;
    uint32_t body_first = 0, body_last = 0;
    std::map<uint32_t, SourceLine> lines;
    std::set<uint32_t> anchors;
    std::set<uint32_t> requested_lines;
    uint64_t memory_bytes() const;
};

constexpr uint32_t investigation_small_body_lines = 40;

SourceChunk make_source_chunk(const std::string &file, const LineIndex &lines,
                              const ScopeRange *scope, uint32_t anchor,
                              const Cli &cli, uint64_t retention_limit);
void merge_source_chunk(SourceChunk &into, const SourceChunk &other);
SourceChunk focus_source_chunk(const SourceChunk &source, const std::set<uint32_t> &anchors,
                               const Cli &cli);

// Level 0: all retained source, 1: signature and local context,
// 2: signature and anchors, 3: locations only. All omissions are explicit.
std::string render_source_chunk(const SourceChunk &chunk, const std::string &id,
                                int level, bool llm);
bool source_chunk_complete(const SourceChunk &chunk, int level);

} // namespace hpr
