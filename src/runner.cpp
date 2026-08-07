#include "runner.hpp"

#include "extract.hpp"
#include "evidence.hpp"
#include "file_io.hpp"
#include "git.hpp"
#include "ident.hpp"
#include "line_index.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "pipeline.hpp"
#include "planner.hpp"
#include "rank.hpp"
#include "scope.hpp"
#include "seen.hpp"
#include "walker.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
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

// Shared per-file buffering for modes that need to look at a file's matches
// again after the whole scan completes (-sample, -hotspots): owns the
// content (the original mmap'd buffer doesn't outlive the walk callback)
// and rebuilds LineIndex/ScopeIndex against the owned copy so pointers stay
// valid. `kept` is left empty by callers that don't need per-match replay.
struct BufferedFile {
    std::string path;
    std::string content;
    hpr::LineIndex idx;
    hpr::ScopeIndex scope;
    bool scope_built = false;
    std::vector<hpr::Match> kept;
};

BufferedFile buffer_file(const std::string &display_name,
                         std::string_view content,
                         const std::string &eff_scope_lang,
                         const hpr::ScopeConfig &user_scope_custom,
                         bool rebuild_scope) {
    BufferedFile bf;
    bf.path = display_name;
    bf.content.assign(content.data(), content.size());
    bf.idx.build(bf.content);
    if (rebuild_scope) {
        hpr::ScopeConfig sc = hpr::resolve_scope_for_file(
            eff_scope_lang, user_scope_custom, display_name);
        if (!sc.anchor_regex.empty()) {
            std::string serr;
            if (bf.scope.build(bf.content, sc, bf.idx, &serr))
                bf.scope_built = true;
        }
    }
    return bf;
}

// Render a buffered file's matches via -elide's logic into an owned string
// instead of stdout, so -budget can measure the size before committing to
// it. Reuses Formatter/on_file_elide verbatim through a memory-backed
// FILE* (open_memstream is POSIX; this codebase already assumes POSIX
// throughout — mmap, fork/execvp, poll).
std::string render_elide_to_string(const BufferedFile &bf,
                                   const hpr::OutputOptions &render_oo,
                                   const hpr::SeenStore *seen,
                                   std::vector<hpr::SeenMark> *marks_out) {
    char *buf = nullptr;
    size_t len = 0;
    FILE *mem = open_memstream(&buf, &len);
    if (!mem) return {};
    {
        hpr::Formatter fmt(render_oo, mem);
        fmt.on_file_elide(bf.path, bf.kept, bf.content, bf.idx,
                          bf.scope_built ? &bf.scope : nullptr, seen, marks_out);
    }
    std::fclose(mem);
    std::string result(buf, len);
    std::free(buf);
    return result;
}

} // namespace

namespace hpr {

int run_search(const Cli &cli) {
    if (cli.patterns.empty()) {
        std::fprintf(stderr, "hprscript: -p <pattern> or -ident <terms> required\n");
        return 2;
    }
    const auto t_start = std::chrono::steady_clock::now();
    ScanStats stats;
    stats.scan_stages = 1;

    std::vector<Pattern> patterns = build_patterns(cli);

    // -ident groups occupy patterns' tail (build_patterns guarantees this);
    // they're matched by scan_identifiers(), never by Vectorscan, so only
    // the regex-backed prefix is compiled. Vectorscan's reported pattern
    // ids are 0..num_regex-1 either way, which is exactly where those
    // patterns sit in the unified `patterns` vector too.
    std::vector<IdentGroup> ident_groups;
    for (const auto &cp : cli.patterns) {
        if (!cp.ident_terms.empty()) ident_groups.push_back(IdentGroup{cp.ident_terms});
    }
    const size_t num_regex = patterns.size() - ident_groups.size();

    if (cli.explain_plan) {
        ExecutionPlan plan;
        plan.mode = "search";
        PlanStage stage;
        stage.id = "scan0";
        stage.sets = {"quick"};
        stage.patterns = patterns.size();
        stage.inputs.insert(stage.inputs.end(), cli.globs.begin(), cli.globs.end());
        stage.inputs.insert(stage.inputs.end(), cli.positional.begin(),
                            cli.positional.end());
        for (const auto &fl : cli.file_lists)
            stage.inputs.push_back(std::string(fl.nul ? "files0:" : "files:") + fl.path);
        if (cli.git_changed) stage.inputs.push_back("git:changed");
        if (cli.git_staged) stage.inputs.push_back("git:staged");
        if (cli.git_untracked) stage.inputs.push_back("git:untracked");
        for (const auto &range : cli.git_ranges)
            stage.inputs.push_back("git:range:" + range);
        if (stage.inputs.empty()) stage.inputs.push_back("<stdin>");
        stage.scope = cli.scope_lang;
        if (stage.scope.empty() &&
            (!cli.scope_pattern.empty() || !cli.in_scopes.empty()))
            stage.scope = "auto";
        plan.scan_stages.push_back(std::move(stage));
        if (!cli.relations.empty())
            plan.postprocess.push_back({"relations", {}});
        if (!cli.file_where.empty())
            plan.postprocess.push_back({"file-filter", {}});
        if (cli.sample_n > 0)
            plan.postprocess.push_back({"representative-sample", {}});
        if (cli.hotspots_n > 0 || cli.budget_bytes > 0)
            plan.postprocess.push_back({"rank-files", {}});
        if (cli.budget_bytes > 0)
            plan.postprocess.push_back({"pack-evidence", {}});
        if (cli.order_by != Cli::OrderBy::None)
            plan.postprocess.push_back({"order", {}});
        if (cli.limit > 0) plan.limits["matches"] = cli.limit;
        if (cli.per_file_limit > 0)
            plan.limits["matches_per_file"] = cli.per_file_limit;
        if (cli.max_output_bytes > 0)
            plan.limits["output_bytes"] = cli.max_output_bytes;
        emit_execution_plan(plan);
        if (cli.plan_only) return 0;
    }

    std::vector<ResolvedRelation> rels;
    if (!resolve_relations(cli.relations, patterns, rels)) return 2;
    const bool any_scope_rel = any_scope_relation(rels);

    FileWhere fw;
    if (!fw.init(cli.file_where, patterns)) return 2;
    std::map<int, std::unordered_map<std::string, uint32_t>> churn_map;
    if (!fw.churn_windows().empty()) {
        std::string cerr;
        if (!build_churn_map(fw.churn_windows(), churn_map, cerr)) {
            std::fprintf(stderr, "hprscript: git: %s\n", cerr.c_str());
            return 2;
        }
    }

    TargetFilter tf;
    if (!tf.init(cli)) return 2;

    Matcher matcher;
    CompileError ce;
    if (num_regex > 0) {
        std::vector<Pattern> regex_patterns(patterns.begin(),
                                            patterns.begin() + num_regex);
        if (!matcher.compile(regex_patterns, &ce)) {
            std::fprintf(stderr, "hprscript: pattern compile failed: %s\n",
                         ce.message.c_str());
            if (ce.pattern_index >= 0 &&
                static_cast<size_t>(ce.pattern_index) < patterns.size()) {
                std::fprintf(stderr, "  in pattern: %s\n",
                             patterns[ce.pattern_index].regexp.c_str());
            }
            return 2;
        }
        stats.matcher_compilations = 1;
        stats.patterns_compiled = num_regex;
    }
    // Identifier groups share the traversal but do not pass through
    // Vectorscan's compiler. They still count as compiled search patterns
    // for execution accounting.
    stats.patterns_compiled += ident_groups.size();

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
    if ((tf.scope_needed() || oo.mode == OutputMode::Elide ||
         cli.budget_bytes > 0) &&
        eff_scope_lang.empty() && cli.scope_pattern.empty())
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
            oo.mode == OutputMode::Absent || oo.mode == OutputMode::Elide) {
            std::fprintf(stderr,
                         "hprscript: -sample requires a per-match output mode "
                         "(default JSONL, -o, -format, -llm) — -elide renders "
                         "whole-file batches and doesn't compose with -sample\n");
            return 2;
        }
    }
    struct SampleRec {
        size_t file_idx;
        Match m;
    };
    std::vector<BufferedFile> sample_files;
    std::vector<SampleRec> sample_recs;
    const size_t SAMPLE_REC_CAP = std::max<size_t>(
        100u * static_cast<size_t>(cli.sample_n > 0 ? cli.sample_n : 1),
        10000u);

    // Hotspot-mode buffering. When hotspots_n > 0 we accumulate the same
    // rarity/coverage/proximity signal script mode's `rank` uses (see
    // src/rank.hpp), one FileRank per file with ≥1 match, then score and
    // emit the top N after the whole scan. Full file content is only
    // buffered under -elide, which needs to re-render the file; JSONL/-llm
    // hotspot rows need nothing but the accumulated RankInput.
    const bool hotspotting = cli.hotspots_n > 0;
    if (hotspotting) {
        if (sampling) {
            std::fprintf(stderr,
                         "hprscript: -hotspots cannot combine with -sample "
                         "(both buffer the scan to pick a subset)\n");
            return 2;
        }
        if (oo.mode != OutputMode::JsonLines && oo.mode != OutputMode::Llm &&
            oo.mode != OutputMode::Elide) {
            std::fprintf(stderr,
                         "hprscript: -hotspots output is JSONL (default), "
                         "-llm, or -elide\n");
            return 2;
        }
    }
    RankInput hs_input;
    if (hotspotting) {
        for (const auto &p : patterns) {
            hs_input.pattern_weights[p.id] = p.weight;
            hs_input.queried_ids.insert(p.id);
        }
        hs_input.total_queried = static_cast<uint32_t>(patterns.size());
    }
    std::vector<BufferedFile> hs_files;
    std::unordered_map<std::string, size_t> hs_file_idx;
    const size_t HOTSPOT_REC_CAP = std::max<size_t>(
        100u * static_cast<size_t>(cli.hotspots_n > 0 ? cli.hotspots_n : 1),
        10000u);
    size_t hs_buffered_matches = 0;

    // Budget-packing mode. Unlike -hotspots (a fixed top-N), -budget doesn't
    // know ahead of time how many files will fit, so every file with ≥1
    // match is ranked and buffered in full (bounded by BUDGET_REC_CAP, same
    // "silently truncates" convention as -sample/-hotspots). It defines its
    // own output shape, so no other output-mode flag may be set.
    const bool budgeting = cli.budget_bytes > 0;
    if (budgeting) {
        if (sampling || hotspotting) {
            std::fprintf(stderr,
                         "hprscript: -budget cannot combine with "
                         "-sample/-hotspots\n");
            return 2;
        }
        if (cli.out_mode_set) {
            std::fprintf(stderr,
                         "hprscript: -budget defines its own output shape — "
                         "drop -j/-f/-c/-o/-format/-absent/-llm/-elide\n");
            return 2;
        }
    }
    RankInput bg_input;
    if (budgeting) {
        for (const auto &p : patterns) {
            bg_input.pattern_weights[p.id] = p.weight;
            bg_input.queried_ids.insert(p.id);
        }
        bg_input.total_queried = static_cast<uint32_t>(patterns.size());
    }
    std::vector<BufferedFile> bg_files;
    std::unordered_map<std::string, size_t> bg_file_idx;
    static constexpr size_t BUDGET_REC_CAP = 20000;
    size_t bg_buffered_matches = 0;
    OutputOptions bg_render_oo;
    if (budgeting) {
        bg_render_oo.mode = OutputMode::Elide;
        bg_render_oo.context_before = cli.context_before;
        bg_render_oo.context_after = cli.context_after;
        bg_render_oo.max_match_bytes = cli.max_match_bytes;
        bg_render_oo.max_context_bytes = cli.max_context_bytes;
        bg_render_oo.max_block_bytes = cli.max_block_bytes;
    }

    // -order-by: sorts -f/-c output instead of streaming it in walk order.
    // `score` needs the same whole-scan RankInput accumulation as
    // -hotspots/-budget; `count`/`path` just need the per-file row buffered.
    // No separate check against -sample/-hotspots/-budget is needed here:
    // each of those already requires an output mode that isn't -f/-c, so
    // they can never reach this point already combined with -order-by.
    const bool ordering = cli.order_by != Cli::OrderBy::None;
    if (ordering && oo.mode != OutputMode::FilesOnly &&
        oo.mode != OutputMode::Counts) {
        std::fprintf(stderr, "hprscript: -order-by requires -f or -c output\n");
        return 2;
    }
    struct OrderRow {
        std::string file;
        uint64_t count;
    };
    std::vector<OrderRow> order_rows;
    RankInput ord_input;
    if (ordering && cli.order_by == Cli::OrderBy::Score) {
        for (const auto &p : patterns) {
            ord_input.pattern_weights[p.id] = p.weight;
            ord_input.queried_ids.insert(p.id);
        }
        ord_input.total_queried = static_cast<uint32_t>(patterns.size());
    }

    // -seen: cross-invocation dedup, only meaningful where there's a
    // "chunk" to collapse (-elide's own mode, or -budget's internal use of
    // it). Loaded once up front; rewritten once at the end with whatever
    // this run actually displayed in full (see SeenMark's doc comment for
    // why -budget only commits marks for its full-render tier).
    const bool seen_active = !cli.seen_path.empty();
    if (seen_active && oo.mode != OutputMode::Elide && !budgeting) {
        std::fprintf(stderr, "hprscript: -seen requires -elide or -budget\n");
        return 2;
    }
    SeenStore seen_store;
    if (seen_active) seen_store.load(cli.seen_path);

    MatchCollector collector(patterns, std::move(rels), cli.git_added_lines,
                             ident_groups);
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
        stats.rows_materialized += kept.size();

        // -file-where: emit this file's matches only when the predicate over
        // its matched-pattern set holds.
        if (fw.active() &&
            !fw.pass(kept, patterns.size(), display_name, churn_map)) {
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
            if (sample_recs.size() >= SAMPLE_REC_CAP) {
                stats.rows_truncated += kept.size();
                if (stats.stop_reason.empty()) stats.stop_reason = "row_cap";
                return true;
            }
            size_t fidx = sample_files.size();
            sample_files.push_back(buffer_file(display_name, content,
                                               eff_scope_lang,
                                               user_scope_custom,
                                               scope_ptr != nullptr));
            stats.buffered_bytes_peak += content.size();
            for (size_t mi = 0; mi < kept.size(); ++mi) {
                if (sample_recs.size() >= SAMPLE_REC_CAP) {
                    stats.rows_truncated += kept.size() - mi;
                    if (stats.stop_reason.empty()) stats.stop_reason = "row_cap";
                    break;
                }
                sample_recs.push_back({fidx, kept[mi]});
            }
            return true;
        }

        if (hotspotting) {
            if (kept.empty()) return true;
            accumulate_rank_input(hs_input, display_name, kept, patterns, idx);
            if (oo.mode == OutputMode::Elide &&
                hs_buffered_matches < HOTSPOT_REC_CAP) {
                BufferedFile bf = buffer_file(display_name, content,
                                              eff_scope_lang, user_scope_custom,
                                              scope_ptr != nullptr);
                bf.kept = kept;
                hs_buffered_matches += kept.size();
                stats.buffered_bytes_peak += content.size();
                hs_file_idx[display_name] = hs_files.size();
                hs_files.push_back(std::move(bf));
            } else if (oo.mode == OutputMode::Elide) {
                stats.rows_truncated += kept.size();
                if (stats.stop_reason.empty()) stats.stop_reason = "row_cap";
            }
            return true;
        }

        if (budgeting) {
            if (kept.empty()) return true;
            accumulate_rank_input(bg_input, display_name, kept, patterns, idx);
            if (bg_buffered_matches < BUDGET_REC_CAP) {
                BufferedFile bf = buffer_file(display_name, content,
                                              eff_scope_lang, user_scope_custom,
                                              scope_ptr != nullptr);
                bf.kept = kept;
                bg_buffered_matches += kept.size();
                stats.buffered_bytes_peak += content.size();
                bg_file_idx[display_name] = bg_files.size();
                bg_files.push_back(std::move(bf));
            } else {
                stats.rows_truncated += kept.size();
                if (stats.stop_reason.empty()) stats.stop_reason = "row_cap";
            }
            return true;
        }

        if (ordering) {
            // Match -f's existing semantics (files with zero matches are
            // omitted) and -c's (every scanned file gets a row, even :0).
            if (oo.mode == OutputMode::FilesOnly && kept.empty()) return true;
            order_rows.push_back({display_name, static_cast<uint64_t>(kept.size())});
            if (cli.order_by == Cli::OrderBy::Score && !kept.empty())
                accumulate_rank_input(ord_input, display_name, kept, patterns, idx);
            return true;
        }

        if (oo.mode == OutputMode::Elide) {
            // Whole-file batch render — -m caps how many of this file's
            // matches participate, but there's no natural mid-file stopping
            // point the way per-match modes have one.
            std::vector<Match> capped = kept;
            if (cli.per_file_limit > 0 &&
                capped.size() > static_cast<size_t>(cli.per_file_limit))
                capped.resize(static_cast<size_t>(cli.per_file_limit));
            if (!capped.empty()) {
                had_match = true;
                std::vector<SeenMark> marks;
                fmt.on_file_elide(display_name, capped, content, idx, scope_ptr,
                                  seen_active ? &seen_store : nullptr,
                                  seen_active ? &marks : nullptr);
                // -elide's render is the final output — nothing measured
                // and discarded like -budget — so every mark commits.
                if (seen_active) for (const auto &m : marks) seen_store.mark(m);
            }
        } else {
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
        stats.bytes_scanned += content.size();
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
            stats.bytes_scanned += mf.view().size();
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
            const BufferedFile &sf = sample_files[r.file_idx];
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
            const BufferedFile &sf = sample_files[r.file_idx];
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

    uint64_t hs_emitted_rows = 0;
    if (hotspotting) {
        std::vector<RankRow> rows = rank_files(hs_input);
        if (rows.size() > static_cast<size_t>(cli.hotspots_n))
            rows.resize(static_cast<size_t>(cli.hotspots_n));
        for (const auto &r : rows) {
            if (oo.mode == OutputMode::Elide) {
                auto it = hs_file_idx.find(r.file);
                if (it == hs_file_idx.end())
                    continue; // buffering cap hit before this file — skip
                const BufferedFile &bf = hs_files[it->second];
                if (bf.kept.empty()) continue;
                std::vector<SeenMark> marks;
                fmt.on_file_elide(bf.path, bf.kept, bf.content, bf.idx,
                                  bf.scope_built ? &bf.scope : nullptr,
                                  seen_active ? &seen_store : nullptr,
                                  seen_active ? &marks : nullptr);
                if (seen_active) for (const auto &m : marks) seen_store.mark(m);
                ++hs_emitted_rows;
            } else if (oo.mode == OutputMode::Llm) {
                std::string line = r.file;
                line += ':';
                line += std::to_string(r.window_lo);
                line += '-';
                line += std::to_string(r.window_hi);
                line += " score=";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", r.score);
                line += buf;
                line += " patterns=";
                for (size_t i = 0; i < r.matched_patterns.size(); ++i) {
                    if (i) line += ',';
                    line += r.matched_patterns[i];
                }
                line += '\n';
                std::fwrite(line.data(), 1, line.size(), stdout);
                ++hs_emitted_rows;
            } else {
                std::string s = "{\"type\":\"hotspot\",\"file\":\"";
                json_escape_to(s, r.file);
                s += "\",\"score\":";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%g", r.score);
                s += buf;
                s += ",\"line_start\":";
                s += std::to_string(r.window_lo);
                s += ",\"line_end\":";
                s += std::to_string(r.window_hi);
                s += ",\"patterns\":[";
                for (size_t i = 0; i < r.matched_patterns.size(); ++i) {
                    if (i) s += ',';
                    s += '"';
                    json_escape_to(s, r.matched_patterns[i]);
                    s += '"';
                }
                s += "]}\n";
                std::fwrite(s.data(), 1, s.size(), stdout);
                ++hs_emitted_rows;
            }
        }
    }

    uint64_t bg_emitted_rows = 0;
    if (budgeting) {
        std::vector<RankRow> rows = rank_files(bg_input);
        int64_t remaining = static_cast<int64_t>(cli.budget_bytes);
        std::vector<std::string> dropped;
        size_t full_count = 0, compact_count = 0;

        auto compact_line = [](const RankRow &r) {
            std::string line = r.file;
            line += ':';
            line += std::to_string(r.window_lo);
            line += '-';
            line += std::to_string(r.window_hi);
            line += " score=";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", r.score);
            line += buf;
            line += " patterns=";
            for (size_t i = 0; i < r.matched_patterns.size(); ++i) {
                if (i) line += ',';
                line += r.matched_patterns[i];
            }
            line += " (compact — full render didn't fit the budget)\n";
            return line;
        };

        for (const auto &r : rows) {
            if (remaining <= 0) { dropped.push_back(r.file); continue; }
            auto it = bg_file_idx.find(r.file);
            if (it == bg_file_idx.end()) { // buffering cap hit — never captured
                dropped.push_back(r.file);
                continue;
            }
            const BufferedFile &bf = bg_files[it->second];
            if (bf.kept.empty()) { dropped.push_back(r.file); continue; }

            // Measured against `seen` for collapse sizing either way, but
            // marks are only committed to the real store if this render is
            // the one that actually ends up on screen — a render measured
            // here and then degraded to compact/dropped must not be
            // recorded as "shown" (see SeenMark's doc comment).
            std::vector<SeenMark> marks;
            std::string full_text = render_elide_to_string(
                bf, bg_render_oo, seen_active ? &seen_store : nullptr,
                seen_active ? &marks : nullptr);
            if (static_cast<int64_t>(full_text.size()) <= remaining) {
                std::fwrite(full_text.data(), 1, full_text.size(), stdout);
                remaining -= static_cast<int64_t>(full_text.size());
                ++full_count;
                ++bg_emitted_rows;
                if (seen_active) for (const auto &m : marks) seen_store.mark(m);
                continue;
            }
            std::string compact = compact_line(r);
            if (static_cast<int64_t>(compact.size()) <= remaining) {
                std::fwrite(compact.data(), 1, compact.size(), stdout);
                remaining -= static_cast<int64_t>(compact.size());
                ++compact_count;
                ++bg_emitted_rows;
                continue;
            }
            dropped.push_back(r.file);
        }

        if (compact_count > 0 || !dropped.empty()) {
            std::string footer = "--- budget: ";
            footer += std::to_string(full_count);
            footer += " file(s) in full, ";
            footer += std::to_string(compact_count);
            footer += " compact, ";
            footer += std::to_string(dropped.size());
            footer += " dropped";
            if (!dropped.empty()) {
                footer += ": ";
                static constexpr size_t kMaxNamed = 10;
                for (size_t i = 0; i < dropped.size() && i < kMaxNamed; ++i) {
                    if (i) footer += ", ";
                    footer += dropped[i];
                }
                if (dropped.size() > kMaxNamed) {
                    footer += " (+";
                    footer += std::to_string(dropped.size() - kMaxNamed);
                    footer += " more)";
                }
            }
            footer += " ---\n";
            std::fwrite(footer.data(), 1, footer.size(), stdout);
        }
    }

    uint64_t ord_emitted = 0;
    if (ordering) {
        std::unordered_map<std::string, double> score_by_file;
        if (cli.order_by == Cli::OrderBy::Score) {
            for (const auto &r : rank_files(ord_input)) score_by_file[r.file] = r.score;
        }
        std::stable_sort(order_rows.begin(), order_rows.end(),
                         [&](const OrderRow &a, const OrderRow &b) {
            switch (cli.order_by) {
                case Cli::OrderBy::Path:
                    return a.file < b.file;
                case Cli::OrderBy::Count:
                    return a.count > b.count; // descending
                case Cli::OrderBy::Score: {
                    double sa = score_by_file.count(a.file) ? score_by_file[a.file] : 0.0;
                    double sb = score_by_file.count(b.file) ? score_by_file[b.file] : 0.0;
                    return sa > sb; // descending; stable_sort keeps walk order on ties
                }
                default:
                    return false;
            }
        });
        for (const auto &r : order_rows) {
            std::string line = r.file;
            if (oo.mode == OutputMode::Counts) {
                line += ':';
                line += std::to_string(r.count);
            }
            line += '\n';
            std::fwrite(line.data(), 1, line.size(), stdout);
            // Mirrors Formatter::emitted()'s existing -f/-c semantics: it
            // counts matches, not rows/files — -c prints a ":0" row for a
            // clean file, but that shouldn't flip -summary/exit code to
            // "found something".
            ord_emitted += r.count;
        }
    }

    if (seen_active) {
        std::string serr;
        if (!seen_store.save(cli.seen_path, &serr)) {
            // The real output already printed successfully — a state-file
            // write failure shouldn't flip the exit code, just warn.
            std::fprintf(stderr, "hprscript: -seen: %s\n", serr.c_str());
        }
    }

    fmt.on_complete();

    if (cli.summary) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
        uint64_t emitted_for_summary = fmt.emitted();
        if (hotspotting) emitted_for_summary = hs_emitted_rows;
        if (budgeting) emitted_for_summary = bg_emitted_rows;
        if (ordering) emitted_for_summary = ord_emitted;
        stats.rows_output = emitted_for_summary;
        emit_summary_record(stats, emitted_for_summary,
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
    if (hotspotting) return hs_emitted_rows > 0 ? 0 : 1;
    if (budgeting) return bg_emitted_rows > 0 ? 0 : 1;
    if (ordering) return ord_emitted > 0 ? 0 : 1;
    return fmt.emitted() > 0 ? 0 : 1;
}

int run_list_scopes(const Cli &cli) {
    const auto t_start = std::chrono::steady_clock::now();
    ScanStats stats;
    stats.scan_stages = 1;
    if (cli.explain_plan) {
        ExecutionPlan plan;
        plan.mode = "list-scopes";
        PlanStage stage;
        stage.id = "scan0";
        stage.sets = {"scopes"};
        stage.inputs.insert(stage.inputs.end(), cli.globs.begin(), cli.globs.end());
        stage.inputs.insert(stage.inputs.end(), cli.positional.begin(),
                            cli.positional.end());
        if (stage.inputs.empty()) stage.inputs.push_back("<stdin>");
        stage.scope = cli.scope_lang.empty() ? "auto" : cli.scope_lang;
        plan.scan_stages.push_back(std::move(stage));
        if (!cli.in_scopes.empty())
            plan.postprocess.push_back({"scope-filter", {}});
        if (cli.limit > 0) plan.limits["rows"] = cli.limit;
        emit_execution_plan(plan);
        if (cli.plan_only) return 0;
    }
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
            ++stats.rows_materialized;
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
        stats.bytes_scanned += content.size();
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
            stats.bytes_scanned += mf.view().size();
            return scan_buf(it.path, mf.view());
        });
    }
    if (hit_limit) stats.stop_reason = "limit";
    stats.rows_output = emitted;
    if (cli.summary) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
        emit_summary_record(stats, emitted, static_cast<uint64_t>(elapsed));
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
    return emitted > 0 ? 0 : 1;
}

} // namespace hpr
