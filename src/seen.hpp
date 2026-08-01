// Cross-invocation "already shown" state for -seen: lets repeated agent
// queries against -elide/-budget skip re-paying tokens for chunks that
// haven't changed since the last run.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hpr {

// FNV-1a over raw bytes — fast, non-cryptographic, good enough for change
// detection (not a security boundary).
uint64_t fnv1a(std::string_view data);

// One remembered chunk: a scope's line range and content hash as of the
// last run that rendered it in full.
struct SeenEntry {
    uint32_t line_start = 0;
    uint32_t line_end = 0;
    uint64_t hash = 0;
};

// A chunk this run examined, recorded whether it collapsed or rendered in
// full — the caller decides whether to commit it (see mark()) once it
// knows the chunk actually made it into the final output. This split
// matters for -budget: it measures a file's full render to decide whether
// the render fits the byte budget, and a measurement that's ultimately
// discarded (degraded to a compact summary or dropped) must not be
// recorded as "shown".
struct SeenMark {
    std::string file;
    uint32_t line_start = 0;
    uint32_t line_end = 0;
    uint64_t hash = 0;
};

class SeenStore {
public:
    // Load `path`'s prior entries. A missing or unreadable file is treated
    // as an empty store (first run) rather than an error — -seen is a soft
    // cache, not a hard contract; a corrupt line is skipped, not fatal.
    void load(const std::string &path);

    // True when `file`'s [line_start,line_end] chunk was recorded last run
    // with this exact hash — i.e. safe to collapse.
    bool seen_unchanged(const std::string &file, uint32_t line_start,
                       uint32_t line_end, uint64_t hash) const;

    // Commit a chunk into this run's state (called only for chunks that
    // actually reached the final output — see SeenMark's doc comment).
    void mark(const std::string &file, uint32_t line_start, uint32_t line_end,
             uint64_t hash);
    void mark(const SeenMark &m) { mark(m.file, m.line_start, m.line_end, m.hash); }

    // Atomically rewrite `path` with this run's marked entries (mkstemp +
    // rename). Entries never marked this run — collapsed away, or simply
    // not visited — are dropped, so the file always reflects exactly what
    // the most recent run actually displayed.
    bool save(const std::string &path, std::string *err) const;

private:
    std::unordered_map<std::string, std::vector<SeenEntry>> prior_;
    std::unordered_map<std::string, std::vector<SeenEntry>> current_;
};

} // namespace hpr
