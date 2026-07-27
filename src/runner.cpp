#include "runner.hpp"

#include "extract.hpp"
#include "file_io.hpp"
#include "git.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "pipeline.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

} // namespace

namespace hpr {

int run_search(const Cli &cli) {
    if (cli.patterns.empty()) {
        std::fprintf(stderr, "hprscript: -p <pattern> required\n");
        return 2;
    }
    const auto t_start = std::chrono::steady_clock::now();
    ScanStats stats;

    std::vector<Pattern> patterns = build_patterns(cli);

    std::vector<ResolvedRelation> rels;
    if (!resolve_relations(cli.relations, patterns, rels)) return 2;
    const bool any_scope_rel = any_scope_relation(rels);

    FileWhere fw;
    if (!fw.init(cli.file_where, patterns)) return 2;

    TargetFilter tf;
    if (!tf.init(cli)) return 2;

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
                           || (!oo.block_open.empty() && !oo.block_close.empty())
                           || cli.records != Cli::RecordMode::None
                           || cli.git_added_lines
                           || tf.lines_needed();

    Walker walker;
    std::unordered_map<std::string, AddedLines> added;
    if (!add_walker_inputs(cli, walker, stats, added)) return 2;

    bool no_inputs = cli.globs.empty() && cli.positional.empty() &&
                     cli.file_lists.empty() && !cli.git_changed &&
                     !cli.git_staged && !cli.git_untracked &&
                     cli.git_ranges.empty();
    bool reading_stdin = no_inputs && !isatty(fileno(stdin));

    // Unknown pack names used to silently disable scope annotation (the
    // cookbook shipped a `-scope py` recipe nobody could tell was a no-op).
    if (!cli.scope_lang.empty() && cli.scope_lang != "auto" &&
        !builtin_scope_pack(cli.scope_lang)) {
        std::fprintf(stderr,
                     "hprscript: unknown -scope pack '%s' (supported: auto, "
                     "go, rust, c, cpp, java, js, ts)\n",
                     cli.scope_lang.c_str());
        return 2;
    }

    // Custom scope-config (used when no built-in pack is selected).
    ScopeConfig user_scope_custom;
    user_scope_custom.anchor_regex = cli.scope_pattern;
    user_scope_custom.open = cli.scope_open;
    user_scope_custom.close = cli.scope_close;
    user_scope_custom.kind = cli.scope_kind;
    // -in-scope implies `-scope auto` when no scope config was given. Side
    // effect: records gain the `enclosing` annotation, which is useful
    // context anyway.
    std::string eff_scope_lang = cli.scope_lang;
    if (tf.scope_needed() && eff_scope_lang.empty() &&
        cli.scope_pattern.empty())
        eff_scope_lang = "auto";
    bool scope_enabled = !eff_scope_lang.empty() ||
                         (!cli.scope_pattern.empty() && !cli.scope_open.empty() &&
                          !cli.scope_close.empty());
    if (any_scope_rel && !scope_enabled) {
        std::fprintf(stderr,
                     "hprscript: -same-scope/-not-same-scope require an active "
                     "-scope (built-in pack or -scope-pattern)\n");
        return 2;
    }
    if (fw.active() && oo.mode == OutputMode::Absent) {
        std::fprintf(stderr,
                     "hprscript: -file-where cannot combine with -absent — "
                     "express absence inside the predicate instead "
                     "(e.g. -file-where 'NOT x' -f)\n");
        return 2;
    }
    if (cli.records != Cli::RecordMode::None) {
        if (oo.mode != OutputMode::Absent) {
            std::fprintf(stderr,
                         "hprscript: -records requires -absent "
                         "(record-level absence)\n");
            return 2;
        }
        if (!rels.empty()) {
            std::fprintf(stderr,
                         "hprscript: -records cannot combine with "
                         "-near/-far/-same-scope relations\n");
            return 2;
        }
    }
    if (cli.git_added_lines) {
        if (!cli.globs.empty() || !cli.positional.empty() ||
            !cli.file_lists.empty()) {
            std::fprintf(stderr,
                         "hprscript: -git-added-lines takes its input from "
                         "git alone — drop -glob/positional paths/"
                         "-files-from\n");
            return 2;
        }
        if (cli.records != Cli::RecordMode::None) {
            std::fprintf(stderr,
                         "hprscript: -git-added-lines cannot combine with "
                         "-records\n");
            return 2;
        }
    }

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

    MatchCollector collector(patterns, std::move(rels), cli.git_added_lines);
    const bool have_rels = !cli.relations.empty();

    auto scan_buf = [&](const std::string &display_name,
                        std::string_view content) -> bool {
        LineIndex idx;
        if (need_idx || scope_enabled || have_rels) idx.build(content);

        ScopeIndex scope;
        const ScopeIndex *scope_ptr = nullptr;
        if (scope_enabled) {
            scope_ptr = build_file_scope(eff_scope_lang, user_scope_custom,
                                         display_name, content, idx, scope);
        }

        std::vector<Match> kept;
        const AddedLines *al = nullptr;
        if (cli.git_added_lines) {
            auto ait = added.find(display_name);
            if (ait != added.end()) al = &ait->second;
        }
        collector.collect(matcher, content, idx, scope_ptr, al, kept);
        tf.apply(kept, idx, scope_ptr);

        stats.matches_seen += kept.size();

        // -file-where: emit this file's matches only when the predicate over
        // its matched-pattern set holds.
        if (fw.active() && !fw.pass(kept, patterns.size())) {
            fmt.on_file_end(display_name, false);
            return true;
        }

        // -records line (with -absent): emit one record per non-empty line
        // lacking each pattern, then skip the per-file emission path (and the
        // Absent-mode file listing) entirely.
        if (cli.records == Cli::RecordMode::Line) {
            uint32_t nlines = idx.line_count();
            std::vector<std::vector<char>> hit(patterns.size());
            for (auto &v : hit) v.assign(nlines + 1, 0);
            for (const auto &mm : kept)
                hit[mm.pattern_index][idx.line_of(mm.from)] = 1;
            for (uint32_t L = 1; L <= nlines; ++L) {
                std::string_view text = idx.line_text(L);
                if (text.empty()) continue; // blank records are skipped
                for (size_t pi = 0; pi < patterns.size(); ++pi) {
                    if (hit[pi][L]) continue;
                    fmt.on_record_absent(display_name, patterns[pi], L, text);
                    if (fmt.over_budget()) {
                        if (stats.stop_reason.empty())
                            stats.stop_reason = "output_budget";
                        return false;
                    }
                    if (cli.limit > 0 &&
                        fmt.emitted() >= static_cast<uint64_t>(cli.limit)) {
                        fmt.mark_limit_hit();
                        if (stats.stop_reason.empty())
                            stats.stop_reason = "limit";
                        return false;
                    }
                }
            }
            return true;
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
                ScopeConfig sc = resolve_scope_for_file(eff_scope_lang,
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

        if (fmt.over_budget()) {
            if (stats.stop_reason.empty()) stats.stop_reason = "output_budget";
            return false;
        }
        if (cli.limit > 0 &&
            fmt.emitted() >= static_cast<uint64_t>(cli.limit)) {
            fmt.mark_limit_hit();
            if (stats.stop_reason.empty()) stats.stop_reason = "limit";
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
        ++stats.files_scanned;
        scan_buf("<stdin>", content);
    } else {
        walker.walk([&](const WalkItem &it) {
            MappedFile mf;
            if (!mf.open(it.path)) {
                ++stats.files_failed;
                if (cli.diagnostics) {
                    emit_warning_record("read_error", it.path);
                } else {
                    std::fprintf(stderr, "hprscript: cannot read %s: %s\n",
                                 it.path.c_str(), std::strerror(errno));
                }
                return true;
            }
            if (looks_binary(mf.view())) {
                ++stats.files_binary;
                if (cli.diagnostics) emit_warning_record("binary_skip", it.path);
                return true;
            }
            ++stats.files_scanned;
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
            if (fmt.over_budget()) {
                if (stats.stop_reason.empty())
                    stats.stop_reason = "output_budget";
                break;
            }
        }
    }

    fmt.on_complete();

    if (cli.summary) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
        emit_summary_record(stats, fmt.emitted(),
                            static_cast<uint64_t>(elapsed));
    }
    if (cli.require_complete &&
        (stats.files_failed > 0 || stats.missing_paths > 0)) {
        std::fprintf(stderr,
                     "hprscript: incomplete scan: %llu unreadable file(s), "
                     "%llu missing listed path(s)\n",
                     (unsigned long long)stats.files_failed,
                     (unsigned long long)stats.missing_paths);
        return 2;
    }

    // Exit code semantics follow grep: 0 if any match emitted (or absent
    // files printed), 1 if no output, 2 already returned earlier on errors.
    return fmt.emitted() > 0 ? 0 : 1;
}

int run_list_scopes(const Cli &cli) {
    ScanStats stats;
    TargetFilter tf;
    if (!tf.init(cli)) return 2;

    // -scope defaults to auto here: listing scopes without a pack makes no
    // sense, and requiring an explicit -scope auto would just be friction.
    std::string eff_scope_lang =
        cli.scope_lang.empty() && cli.scope_pattern.empty() ? "auto"
                                                            : cli.scope_lang;
    if (!eff_scope_lang.empty() && eff_scope_lang != "auto" &&
        !builtin_scope_pack(eff_scope_lang)) {
        std::fprintf(stderr,
                     "hprscript: unknown -scope pack '%s' (supported: auto, "
                     "go, rust, c, cpp, java, js, ts)\n",
                     eff_scope_lang.c_str());
        return 2;
    }
    ScopeConfig user_scope_custom;
    user_scope_custom.anchor_regex = cli.scope_pattern;
    user_scope_custom.open = cli.scope_open;
    user_scope_custom.close = cli.scope_close;
    user_scope_custom.kind = cli.scope_kind;

    Walker walker;
    std::unordered_map<std::string, AddedLines> added;
    if (!add_walker_inputs(cli, walker, stats, added)) return 2;

    const bool llm = cli.out_mode_set && cli.out_mode == OutputMode::Llm;
    uint64_t emitted = 0;
    bool hit_limit = false;

    auto scan_buf = [&](const std::string &display_name,
                        std::string_view content) -> bool {
        LineIndex idx;
        idx.build(content);
        ScopeIndex scope;
        const ScopeIndex *sp = build_file_scope(eff_scope_lang,
                                                user_scope_custom,
                                                display_name, content, idx,
                                                scope);
        if (!sp) return true;
        for (const ScopeRange &r : sp->all()) {
            if (tf.scope_needed() && !tf.scope_matches(r)) continue;
            if (llm) {
                std::printf("%s:%u-%u %s %s\n", display_name.c_str(),
                            r.line_start, r.line_end, r.kind.c_str(),
                            r.name.c_str());
            } else {
                std::string out = "{\"type\":\"scope\",\"file\":\"";
                json_escape_to(out, display_name);
                out += "\",\"name\":\"";
                json_escape_to(out, r.name);
                out += "\",\"kind\":\"";
                json_escape_to(out, r.kind);
                out += "\",\"line_start\":" + std::to_string(r.line_start);
                out += ",\"line_end\":" + std::to_string(r.line_end);
                out += "}\n";
                std::fputs(out.c_str(), stdout);
            }
            ++emitted;
            if (cli.limit > 0 &&
                emitted >= static_cast<uint64_t>(cli.limit)) {
                hit_limit = true;
                return false;
            }
        }
        return true;
    };

    bool no_inputs = cli.globs.empty() && cli.positional.empty() &&
                     cli.file_lists.empty() && !cli.git_changed &&
                     !cli.git_staged && !cli.git_untracked &&
                     cli.git_ranges.empty();
    if (no_inputs && !isatty(fileno(stdin))) {
        std::string content;
        if (!read_stdin(content)) {
            std::fprintf(stderr, "hprscript: failed to read stdin\n");
            return 2;
        }
        ++stats.files_scanned;
        scan_buf("<stdin>", content);
    } else {
        walker.walk([&](const WalkItem &it) {
            MappedFile mf;
            if (!mf.open(it.path)) {
                ++stats.files_failed;
                if (cli.diagnostics) {
                    emit_warning_record("read_error", it.path);
                } else {
                    std::fprintf(stderr, "hprscript: cannot read %s: %s\n",
                                 it.path.c_str(), std::strerror(errno));
                }
                return true;
            }
            if (looks_binary(mf.view())) {
                ++stats.files_binary;
                if (cli.diagnostics) emit_warning_record("binary_skip", it.path);
                return true;
            }
            ++stats.files_scanned;
            return scan_buf(it.path, mf.view());
        });
    }
    (void)hit_limit;
    if (cli.require_complete &&
        (stats.files_failed > 0 || stats.missing_paths > 0)) {
        std::fprintf(stderr,
                     "hprscript: incomplete scan: %llu unreadable file(s), "
                     "%llu missing listed path(s)\n",
                     (unsigned long long)stats.files_failed,
                     (unsigned long long)stats.missing_paths);
        return 2;
    }
    return emitted > 0 ? 0 : 1;
}

} // namespace hpr
