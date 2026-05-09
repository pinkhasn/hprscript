#include "runner.hpp"

#include "extract.hpp"
#include "file_io.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

// Canonical "shape" of a line: identifier-like runs collapsed to `_`,
// whitespace runs collapsed to a single space. Used by -sample to dedupe
// near-identical match contexts (e.g. `x := foo()` vs `y := bar()` → `_ := _()`).
std::string normalise_shape(std::string_view line) {
    std::string s;
    s.reserve(line.size());
    bool in_ident = false;
    bool prev_space = false;
    for (char c : line) {
        bool is_id = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        if (is_id) {
            if (!in_ident) { s += '_'; in_ident = true; prev_space = false; }
            continue;
        }
        in_ident = false;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prev_space) { s += ' '; prev_space = true; }
        } else {
            s += c;
            prev_space = false;
        }
    }
    return s;
}

bool looks_binary(std::string_view content) {
    size_t n = std::min<size_t>(content.size(), 512);
    for (size_t i = 0; i < n; ++i) {
        if (content[i] == '\0') return true;
    }
    return false;
}

} // namespace

namespace hpr {

int run_search(const Cli &cli) {
    if (cli.patterns.empty()) {
        std::fprintf(stderr, "hprscript: -p <pattern> required\n");
        return 2;
    }

    // Build pattern list.
    std::vector<Pattern> patterns;
    patterns.reserve(cli.patterns.size());
    for (size_t i = 0; i < cli.patterns.size(); ++i) {
        Pattern p;
        p.id = "p" + std::to_string(i);
        p.regexp = cli.patterns[i].regexp;
        p.case_insensitive = cli.patterns[i].case_insensitive;
        p.word_boundary = cli.word_boundary;
        p.utf8 = !cli.no_utf8;
        p.ucp = !cli.no_utf8 && cli.ucp;
        p.extract_names = cli.patterns[i].extract_names;
        patterns.push_back(std::move(p));
    }

    // Resolve relation a/b strings to pattern indices.
    struct ResolvedRelation {
        Cli::RelationKind kind;
        uint32_t a_idx, b_idx;
        int lines;
    };
    std::vector<ResolvedRelation> rels;
    auto resolve_pat = [&](const std::string &id, uint32_t &out) -> bool {
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (patterns[i].id == id) { out = static_cast<uint32_t>(i); return true; }
        }
        // Numeric form: "0", "1", …
        char *end = nullptr;
        long n = std::strtol(id.c_str(), &end, 10);
        if (end != id.c_str() && *end == '\0' && n >= 0 &&
            static_cast<size_t>(n) < patterns.size()) {
            out = static_cast<uint32_t>(n);
            return true;
        }
        return false;
    };
    for (const auto &r : cli.relations) {
        ResolvedRelation rr;
        rr.kind = r.kind;
        rr.lines = r.lines;
        if (!resolve_pat(r.a, rr.a_idx) || !resolve_pat(r.b, rr.b_idx)) {
            std::fprintf(stderr,
                         "hprscript: -near/-far: unknown pattern in '%s:%s:%d'\n",
                         r.a.c_str(), r.b.c_str(), r.lines);
            return 2;
        }
        rels.push_back(rr);
    }

    Matcher matcher;
    CompileError ce;
    if (!matcher.compile(patterns, &ce)) {
        std::fprintf(stderr, "hprscript: pattern compile failed: %s\n",
                     ce.message.c_str());
        if (ce.pattern_index >= 0 &&
            static_cast<size_t>(ce.pattern_index) < patterns.size()) {
            std::fprintf(stderr, "  in pattern: %s\n",
                         patterns[ce.pattern_index].regexp.c_str());
        }
        return 2;
    }

    ExtractTable extract_table;
    {
        std::string ee;
        int ee_idx = -1;
        if (!extract_table.build(patterns, &ee, &ee_idx)) {
            std::fprintf(stderr, "hprscript: %s\n", ee.c_str());
            if (ee_idx >= 0 && static_cast<size_t>(ee_idx) < patterns.size()) {
                std::fprintf(stderr, "  in pattern: %s\n",
                             patterns[ee_idx].regexp.c_str());
            }
            return 2;
        }
    }

    OutputOptions oo;
    oo.mode = cli.out_mode;
    oo.format_template = cli.format_template;
    oo.context_before = cli.context_before;
    oo.context_after = cli.context_after;
    oo.block_open = cli.block_open;
    oo.block_close = cli.block_close;
    oo.max_match_bytes = cli.max_match_bytes;
    oo.max_context_bytes = cli.max_context_bytes;
    oo.max_block_bytes = cli.max_block_bytes;
    oo.max_output_bytes = cli.max_output_bytes;
    oo.extract_table = extract_table.any() ? &extract_table : nullptr;
    oo.pattern_count = patterns.size();
    oo.global_limit = cli.limit;
    Formatter fmt(oo, stdout);
    // Block extraction populates `block_line_start`/`_end` from the line
    // index, so we need it whenever block delimiters are configured (even
    // for output modes that wouldn't otherwise build it).
    const bool need_idx = needs_line_index(oo.mode)
                           || (!oo.block_open.empty() && !oo.block_close.empty());

    Walker walker;
    for (const auto &g : cli.globs) walker.add_scan(g);
    for (const auto &p : cli.positional) walker.add_scan(p);
    for (const auto &e : cli.excludes) walker.add_exclude(e);

    bool no_inputs = cli.globs.empty() && cli.positional.empty();
    bool reading_stdin = no_inputs && !isatty(fileno(stdin));

    // Reused match buffer to avoid reallocating per file.
    std::vector<Match> raw;

    // Custom scope-config (used when no built-in pack is selected).
    ScopeConfig user_scope_custom;
    user_scope_custom.anchor_regex = cli.scope_pattern;
    user_scope_custom.open = cli.scope_open;
    user_scope_custom.close = cli.scope_close;
    user_scope_custom.kind = cli.scope_kind;
    bool scope_enabled = !cli.scope_lang.empty() ||
                         (!cli.scope_pattern.empty() && !cli.scope_open.empty() &&
                          !cli.scope_close.empty());

    // Sample-mode buffering. When sample_n > 0 we collect kept matches into a
    // per-file table, then post-process at end to pick stratified
    // representatives. Cap matches buffered to bound memory.
    const bool sampling = cli.sample_n > 0;
    if (sampling) {
        // Sample is incompatible with output modes that don't emit per-match
        // data — let the user know rather than silently doing nothing.
        if (oo.mode == OutputMode::FilesOnly || oo.mode == OutputMode::Counts ||
            oo.mode == OutputMode::Absent) {
            std::fprintf(stderr,
                         "hprscript: -sample requires a per-match output mode (default JSONL, -o, -format)\n");
            return 2;
        }
    }
    struct SampleFile {
        std::string path;
        std::string content;
        LineIndex idx;
        ScopeIndex scope;
        bool scope_built = false;
    };
    struct SampleRec {
        size_t file_idx;
        Match m;
    };
    std::vector<SampleFile> sample_files;
    std::vector<SampleRec> sample_recs;
    const size_t SAMPLE_REC_CAP = std::max<size_t>(
        100u * static_cast<size_t>(cli.sample_n > 0 ? cli.sample_n : 1),
        10000u);

    auto scan_buf = [&](const std::string &display_name,
                        std::string_view content) -> bool {
        LineIndex idx;
        if (need_idx || scope_enabled || !rels.empty()) idx.build(content);

        ScopeIndex scope;
        const ScopeIndex *scope_ptr = nullptr;
        if (scope_enabled) {
            ScopeConfig sc = resolve_scope_for_file(cli.scope_lang,
                                                    user_scope_custom,
                                                    display_name);
            if (!sc.anchor_regex.empty()) {
                std::string serr;
                if (!scope.build(content, sc, idx, &serr)) {
                    std::fprintf(stderr, "hprscript: %s\n", serr.c_str());
                } else {
                    scope_ptr = &scope;
                }
            }
        }

        // Hyperscan reports every accepting position, so a regex like
        // `func\w+` against "func main" yields matches at to=5,6,7…10.
        // Collect raw matches and post-process to leftmost-longest
        // non-overlapping per pattern (grep-style).
        raw.clear();
        Matcher::MatchCb cb = [&](const Match &m) -> bool {
            raw.push_back(m);
            return true;
        };
        matcher.scan(content, cb);

        // Single sort-based dedup pass: by (pattern, from, -to). Within a
        // pattern, after this sort the longest match at each `from` comes
        // first, and any later match whose `from` lies before the previous
        // kept match's `to` is overlapping (skip).
        std::sort(raw.begin(), raw.end(), [](const Match &a, const Match &b) {
            if (a.pattern_index != b.pattern_index)
                return a.pattern_index < b.pattern_index;
            if (a.from != b.from) return a.from < b.from;
            return a.to > b.to;
        });
        std::vector<Match> kept;
        kept.reserve(raw.size());
        uint32_t cur_pat = UINT32_MAX;
        uint64_t last_to = 0;
        for (const auto &m : raw) {
            if (m.pattern_index != cur_pat) {
                cur_pat = m.pattern_index;
                last_to = 0;
            }
            if (m.from < last_to) continue;
            last_to = m.to;
            kept.push_back(m);
        }
        // Final emission order: by (from, pat) globally so multi-pattern
        // results interleave sensibly.
        if (patterns.size() > 1) {
            std::sort(kept.begin(), kept.end(), [](const Match &a, const Match &b) {
                if (a.from != b.from) return a.from < b.from;
                return a.pattern_index < b.pattern_index;
            });
        }

        // Apply -near / -far filters. Rebuild a per-pattern sorted line list
        // and walk each surviving match against every relation it's the `a`
        // side of. ANDed: any failed predicate drops the match.
        if (!rels.empty() && !kept.empty()) {
            std::vector<std::vector<uint32_t>> lines_by_pat(patterns.size());
            for (const auto &mm : kept)
                lines_by_pat[mm.pattern_index].push_back(idx.line_of(mm.from));
            for (auto &v : lines_by_pat) std::sort(v.begin(), v.end());
            std::vector<Match> filtered;
            filtered.reserve(kept.size());
            for (const auto &mm : kept) {
                uint32_t mline = idx.line_of(mm.from);
                bool drop = false;
                for (const auto &r : rels) {
                    if (r.a_idx != mm.pattern_index) continue;
                    const auto &v = lines_by_pat[r.b_idx];
                    // Lower bound for mline - r.lines (clamped at 1)
                    uint32_t lo = (mline > static_cast<uint32_t>(r.lines))
                                  ? mline - static_cast<uint32_t>(r.lines) : 1;
                    uint32_t hi = mline + static_cast<uint32_t>(r.lines);
                    auto lit = std::lower_bound(v.begin(), v.end(), lo);
                    bool found = false;
                    if (r.a_idx == r.b_idx) {
                        // Exclude self (this match's own line — counts only when
                        // the pattern's only contribution to that range is itself).
                        // We approximate: if any element in [lo,hi] differs from
                        // mline, found=true; else found requires count>1 at mline.
                        for (auto it = lit; it != v.end() && *it <= hi; ++it) {
                            if (*it != mline) { found = true; break; }
                        }
                        if (!found) {
                            // Multiple matches on same line?
                            size_t cnt = 0;
                            for (auto it = lit; it != v.end() && *it <= hi; ++it)
                                if (*it == mline) ++cnt;
                            if (cnt > 1) found = true;
                        }
                    } else {
                        for (auto it = lit; it != v.end() && *it <= hi; ++it) {
                            found = true; break;
                        }
                    }
                    if (r.kind == Cli::RelationKind::Near && !found) { drop = true; break; }
                    if (r.kind == Cli::RelationKind::Far  &&  found) { drop = true; break; }
                }
                if (!drop) filtered.push_back(mm);
            }
            kept = std::move(filtered);
        }

        bool had_match = false;
        if (sampling) {
            if (kept.empty()) return true;
            if (sample_recs.size() >= SAMPLE_REC_CAP) return true;
            // Take ownership of file content + indices the sampler will need.
            SampleFile sf;
            sf.path = display_name;
            sf.content.assign(content.data(), content.size());
            sf.idx.build(sf.content);
            if (scope_ptr) {
                // Re-build against the owned content so pointers stay valid.
                ScopeConfig sc = resolve_scope_for_file(cli.scope_lang,
                                                        user_scope_custom,
                                                        display_name);
                if (!sc.anchor_regex.empty()) {
                    std::string serr;
                    if (sf.scope.build(sf.content, sc, sf.idx, &serr)) {
                        sf.scope_built = true;
                    }
                }
            }
            size_t fidx = sample_files.size();
            sample_files.push_back(std::move(sf));
            for (const auto &m : kept) {
                if (sample_recs.size() >= SAMPLE_REC_CAP) break;
                sample_recs.push_back({fidx, m});
            }
            return true;
        }

        uint64_t per_file = 0;
        for (const auto &m : kept) {
            had_match = true;
            ++per_file;
            const Pattern &pat = patterns[m.pattern_index];
            fmt.on_match(display_name, pat, m, content, idx, scope_ptr);
            if (fmt.over_budget()) break;
            if (cli.per_file_limit > 0 &&
                per_file >= static_cast<uint64_t>(cli.per_file_limit)) break;
            if (cli.limit > 0 &&
                fmt.emitted() >= static_cast<uint64_t>(cli.limit)) break;
        }
        fmt.on_file_end(display_name, had_match);

        if (fmt.over_budget()) return false;
        if (cli.limit > 0 &&
            fmt.emitted() >= static_cast<uint64_t>(cli.limit)) {
            fmt.mark_limit_hit();
            return false;
        }
        return true;
    };

    if (reading_stdin) {
        std::string content;
        if (!read_stdin(content)) {
            std::fprintf(stderr, "hprscript: failed to read stdin\n");
            return 2;
        }
        scan_buf("<stdin>", content);
    } else {
        walker.walk([&](const WalkItem &it) {
            MappedFile mf;
            if (!mf.open(it.path)) {
                std::fprintf(stderr, "hprscript: cannot read %s: %s\n",
                             it.path.c_str(), std::strerror(errno));
                return true;
            }
            if (looks_binary(mf.view())) return true; // skip silently
            return scan_buf(it.path, mf.view());
        });
    }

    if (sampling) {
        // Bucket records by (file_idx, shape). Order of first-seen buckets is
        // preserved per file so the round-robin emits in walk order.
        struct Bucket { size_t file_idx; std::vector<size_t> rec_indices; };
        std::vector<Bucket> buckets;
        std::map<std::pair<size_t, std::string>, size_t> bucket_of;

        for (size_t ri = 0; ri < sample_recs.size(); ++ri) {
            const auto &r = sample_recs[ri];
            const SampleFile &sf = sample_files[r.file_idx];
            uint32_t line = sf.idx.line_of(r.m.from);
            std::string_view ltext = sf.idx.line_text(line);
            std::string shape = normalise_shape(ltext.data() ? ltext : std::string_view{});
            auto key = std::make_pair(r.file_idx, shape);
            auto it = bucket_of.find(key);
            if (it == bucket_of.end()) {
                bucket_of.emplace(std::move(key), buckets.size());
                buckets.push_back(Bucket{r.file_idx, {ri}});
            } else {
                buckets[it->second].rec_indices.push_back(ri);
            }
        }

        // Round-robin per file. We keep an index into each file's bucket list
        // and advance one bucket per file per pass; within a bucket we pop one
        // record at a time and move on to the next bucket on the next visit.
        std::map<size_t, std::vector<size_t>> file_buckets;
        for (size_t bi = 0; bi < buckets.size(); ++bi)
            file_buckets[buckets[bi].file_idx].push_back(bi);

        std::vector<size_t> file_order;
        file_order.reserve(file_buckets.size());
        for (size_t i = 0; i < sample_files.size(); ++i)
            if (file_buckets.count(i)) file_order.push_back(i);

        std::map<size_t, size_t> next_bucket_in_file;
        std::map<size_t, size_t> next_rec_in_bucket;

        std::vector<size_t> selected;
        selected.reserve(static_cast<size_t>(cli.sample_n));
        size_t want = static_cast<size_t>(cli.sample_n);

        bool progress = true;
        while (selected.size() < want && progress) {
            progress = false;
            for (size_t fi : file_order) {
                if (selected.size() >= want) break;
                auto &bvec = file_buckets[fi];
                size_t &b_idx = next_bucket_in_file[fi];
                while (b_idx < bvec.size()) {
                    size_t bid = bvec[b_idx];
                    auto &recs = buckets[bid].rec_indices;
                    size_t &r_idx = next_rec_in_bucket[bid];
                    if (r_idx < recs.size()) {
                        selected.push_back(recs[r_idx++]);
                        progress = true;
                        break;
                    }
                    ++b_idx;
                }
            }
        }

        for (size_t ri : selected) {
            const auto &r = sample_recs[ri];
            const SampleFile &sf = sample_files[r.file_idx];
            const Pattern &pat = patterns[r.m.pattern_index];
            fmt.on_match(sf.path, pat, r.m, sf.content, sf.idx,
                         sf.scope_built ? &sf.scope : nullptr);
            if (fmt.over_budget()) break;
        }
    }

    fmt.on_complete();

    // Exit code semantics follow grep: 0 if any match emitted (or absent
    // files printed), 1 if no output, 2 already returned earlier on errors.
    return fmt.emitted() > 0 ? 0 : 1;
}

} // namespace hpr
