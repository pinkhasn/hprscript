#include "investigation_report.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace hpr {

const char *category_name(EvidenceCategory c) {
    static const char *names[] = {"seed_implementation", "related_helper", "caller",
                                  "associated_test", "configuration", "documentation", "other"};
    return names[c];
}

namespace {

std::string q(std::string_view s) { return '"' + json_escape(s) + '"'; }
std::string boolean(bool b) { return b ? "true" : "false"; }
std::string strings(const std::vector<std::string> &v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ','; s += q(v[i]); }
    return s + ']';
}
std::string numbers(const std::vector<uint64_t> &v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ','; s += std::to_string(v[i]); }
    return s + ']';
}

std::string origins(const std::vector<EvidenceOrigin> &v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ',';
        s += "{\"source_row\":" + std::to_string(v[i].source_row) +
             ",\"seed_pattern\":" + std::to_string(v[i].seed) +
             ",\"file\":" + q(v[i].file) + ",\"line\":" + std::to_string(v[i].line) +
             ",\"association\":" + q(v[i].kind) + '}';
    }
    return s + ']';
}

std::string evidence_record(const InvestigationEvidence &e, const std::string &chunk, int level, bool llm) {
    const auto &r = e.row;
    const auto &c = e.classification;
    if (llm) {
        std::string s = (e.ref.empty() ? r.file + ":" + std::to_string(r.line) : e.ref) +
            " [" + category_name(e.category) + "; " + c.classification +
            ", confidence=" + c.confidence + ", method=" + c.method + "] row=" +
            std::to_string(r.row_id) + " source=" + chunk +
            (r.derived_value.empty() ? "" : " derived=" + r.derived_value) + "\n";
        if (!e.origins.empty()) {
            s += "  association:";
            for (const auto &origin : e.origins)
                s += " " + origin.kind + " via " + origin.file + ":" +
                     std::to_string(origin.line) + " (seed-row=" + std::to_string(origin.source_row) + ")";
            s += "\n";
        }
        if (e.ambiguous) s += "  ambiguous target: multiple lexical definitions; no resolved call target\n";
        return s;
    }
    std::string_view context = r.context;
    size_t end = level >= 3 ? 0 : level == 2 ? std::min<size_t>(context.size(), 256) : context.size();
    while (end && end < context.size() && (static_cast<unsigned char>(context[end]) & 0xc0u) == 0x80u) --end;
    context = context.substr(0, end);
    std::string s = "{\"type\":\"investigation-evidence\",\"row_id\":" + std::to_string(r.row_id) +
        ",\"file\":" + q(r.file) + ",\"line\":" + std::to_string(r.line) +
        ",\"column\":" + std::to_string(r.column) + ",\"pattern_id\":" + q(r.pattern_id) +
        ",\"classification\":" + q(c.classification) + ",\"confidence\":" + q(c.confidence) +
        ",\"method\":" + q(c.method) + ",\"context\":" + q(context) +
        ",\"file_roles\":" + strings(e.file_roles) + ",\"category\":" + q(category_name(e.category)) +
        ",\"stage\":" + q(e.derived ? "followup" : "seed") + ",\"source_chunk_id\":" + q(chunk);
    if (context.size() < r.context.size()) s += ",\"context_truncated\":true,\"context_omitted_bytes\":" +
                                                std::to_string(r.context.size() - context.size());
    if (!r.derived_value.empty()) s += ",\"derived_value\":" + q(r.derived_value);
    if (!r.derived_source_rows.empty()) s += ",\"derived_source_rows\":" + numbers(r.derived_source_rows);
    if (!e.origins.empty()) s += ",\"associations\":" + origins(e.origins);
    if (!e.ref.empty()) s += ",\"ref\":" + q(e.ref);
    if (e.ambiguous) s += ",\"ambiguous\":true";
    return s + "}\n";
}

struct Selection {
    std::vector<size_t> rows;
    std::map<std::string, int> levels;
    size_t files = 0, scopes = 0, related = 0, warnings = 0;
};

} // namespace

int emit_investigation_report(const Cli &cli, InvestigationReport &r, uint64_t elapsed_ms) {
    const bool llm = cli.out_mode == OutputMode::Llm;
    uint64_t budget = cli.investigate.evidence_budget;
    if (cli.max_output_bytes && (!budget || cli.max_output_bytes < budget)) budget = cli.max_output_bytes;
    std::map<std::string, std::string> chunk_ids;
    for (const auto &[key, chunk] : r.chunks) chunk_ids[key] = "chunk-" + std::to_string(chunk_ids.size() + 1);

    std::vector<std::string> files, scopes, related;
    for (const auto &file : r.ranked_files) {
        const auto roles = classify_file_roles(file.file);
        if (llm) {
            files.push_back(file.file + " score=" + std::to_string(file.score) +
                " roles=" + strings(roles.roles) + "\n");
        } else {
            files.push_back("{\"type\":\"investigation-file\",\"file\":" + q(file.file) +
                ",\"score\":" + std::to_string(file.score) + ",\"roles\":" + strings(roles.roles) +
                ",\"role_method\":\"path-heuristic\",\"best_line_start\":" + std::to_string(file.window_lo) +
                ",\"best_line_end\":" + std::to_string(file.window_hi) + "}\n");
        }
    }
    std::map<std::pair<std::string, uint64_t>, std::pair<ScopeRef, uint64_t>> scope_rows;
    for (const auto &e : r.evidence) {
        if (!e.row.enclosing) continue;
        auto &entry = scope_rows[{e.row.file, e.row.enclosing->from}];
        entry.first = *e.row.enclosing;
        ++entry.second;
    }
    for (const auto &[key, entry] : scope_rows) {
        const auto &s = entry.first;
        if (llm) scopes.push_back(key.first + ":" + std::to_string(s.line_start) + " " + s.kind + " " + s.name + "\n");
        else scopes.push_back("{\"type\":\"investigation-scope\",\"file\":" + q(key.first) +
            ",\"name\":" + q(s.name) + ",\"kind\":" + q(s.kind) +
            ",\"line_start\":" + std::to_string(s.line_start) + ",\"line_end\":" + std::to_string(s.line_end) +
            ",\"matches\":" + std::to_string(entry.second) + "}\n");
    }
    std::vector<const InvestigationCandidate *> ranked_candidates;
    for (const auto &c : r.candidates) ranked_candidates.push_back(&c);
    std::sort(ranked_candidates.begin(), ranked_candidates.end(), [](const auto *a, const auto *b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        if (a->score != b->score) return a->score > b->score;
        return a->identifier < b->identifier;
    });
    for (const auto *c : ranked_candidates) {
        if (llm) related.push_back(c->identifier + " score=" + std::to_string(c->score) +
            " same_scope=" + std::to_string(c->same_scope_hits) + " files=" + std::to_string(c->corpus_files) +
            (r.followup_ran ? " (corpus searched)\n" : " (seed files only)\n"));
        else related.push_back("{\"type\":\"investigation-related\",\"identifier\":" + q(c->identifier) +
            ",\"score\":" + std::to_string(c->score) + ",\"same_scope_hits\":" + std::to_string(c->same_scope_hits) +
            ",\"same_window_hits\":" + std::to_string(c->same_window_hits) + ",\"same_file_hits\":" +
            std::to_string(c->same_file_hits) + ",\"seed_files\":" + std::to_string(c->seed_files) +
            ",\"corpus_files\":" + std::to_string(c->corpus_files) + ",\"corpus_searched\":" +
            boolean(r.followup_ran) + ",\"origins\":" + origins(c->origins) + "}\n");
    }
    const size_t file_limit = std::min(files.size(), size_t(cli.investigate.top_files));
    const size_t scope_limit = std::min(scopes.size(), size_t(cli.investigate.top_scopes));
    const size_t related_limit = std::min(related.size(), size_t(cli.investigate.related));

    // Category reservation, then fair selection across seed patterns within
    // each category. Repeated rows from one seed cannot monopolize anchors.
    std::array<std::vector<size_t>, CategoryCount> queues;
    for (size_t i = 0; i < r.evidence.size(); ++i) queues[r.evidence[i].category].push_back(i);
    auto priority = [&](size_t i) {
        int p = 3;
        for (const auto &o : r.evidence[i].origins) p = std::min(p, o.priority);
        return p;
    };
    auto association_score = [&](size_t i) {
        for (const auto &c : r.candidates)
            if (r.evidence[i].row.derived_value == c.identifier) return c.score;
        return 0.0;
    };
    for (auto &queue : queues)
        std::sort(queue.begin(), queue.end(), [&](size_t a, size_t b) {
            const bool ad = r.evidence[a].classification.classification == "probable_definition";
            const bool bd = r.evidence[b].classification.classification == "probable_definition";
            if (ad != bd && (r.evidence[a].category == SeedDefinition || r.evidence[a].category == Dependency)) return ad;
            if (priority(a) != priority(b)) return priority(a) < priority(b);
            if (r.evidence[a].ambiguous != r.evidence[b].ambiguous) return !r.evidence[a].ambiguous;
            if (association_score(a) != association_score(b)) return association_score(a) > association_score(b);
            const auto &x = r.evidence[a].row, &y = r.evidence[b].row;
            return std::tie(x.file, x.from, x.pattern_index) < std::tie(y.file, y.from, y.pattern_index);
        });
    std::vector<size_t> order;
    std::array<std::map<uint32_t, size_t>, CategoryCount> seed_usage;
    bool more = true;
    while (more) {
        more = false;
        for (size_t cat = 0; cat < CategoryCount; ++cat) {
            auto &queue = queues[cat];
            if (queue.empty()) continue;
            more = true;
            auto usage = [&](size_t i) {
                const auto &e = r.evidence[i];
                size_t n = seed_usage[cat][e.row.pattern_index];
                if (e.derived && !e.origins.empty()) {
                    n = SIZE_MAX;
                    for (const auto &o : e.origins) n = std::min(n, seed_usage[cat][o.seed]);
                }
                return n;
            };
            auto best = std::min_element(queue.begin(), queue.end(), [&](size_t a, size_t b) { return usage(a) < usage(b); });
            size_t i = *best;
            queue.erase(best);
            order.push_back(i);
            const auto &e = r.evidence[i];
            if (e.derived) { for (const auto &o : e.origins) ++seed_usage[cat][o.seed]; }
            else ++seed_usage[cat][e.row.pattern_index];
        }
    }
    const size_t anchor_limit = std::min(order.size(), size_t(cli.investigate.examples));
    const bool scan_complete = !r.stats.files_failed && !r.stats.missing_paths;
    const bool expansion_complete = !r.candidate_omissions && !r.memory_omissions && !r.followup_failures;
    auto serialize = [&](const Selection &s) {
        std::map<std::string, std::set<uint32_t>> anchors;
        for (size_t i : s.rows) anchors[r.evidence[i].chunk_key].insert(r.evidence[i].row.line);
        std::map<std::string, SourceChunk> chunks;
        for (const auto &[key, selected_anchors] : anchors)
            chunks.emplace(key, focus_source_chunk(r.chunks.at(key), selected_anchors, cli));
        uint64_t source_omissions = 0;
        for (const auto &[key, level] : s.levels)
            if (!source_chunk_complete(chunks.at(key), level)) ++source_omissions;
        const bool output_complete = !r.retention_omissions && !r.memory_omissions &&
            s.rows.size() == r.evidence.size() && s.files == files.size() &&
            s.scopes == scopes.size() && s.related == related.size() &&
            !source_omissions && s.warnings == r.warnings.size() && !r.warning_omissions;
        const bool complete = scan_complete && expansion_complete && output_complete;
        std::array<uint64_t, CategoryCount> emitted{};
        for (size_t i : s.rows) ++emitted[r.evidence[i].category];
        std::vector<std::string> reasons;
        if (!scan_complete) reasons.push_back("input_failure");
        if (r.candidate_omissions) reasons.push_back("adaptive_pattern_cap");
        if (r.memory_omissions) reasons.push_back("memory_cap");
        if (r.retention_omissions) reasons.push_back("retention_cap");
        if (r.evidence.size() > anchor_limit || files.size() > file_limit ||
            scopes.size() > scope_limit || related.size() > related_limit) reasons.push_back("section_limits");
        if (s.rows.size() < anchor_limit || s.files < file_limit || s.scopes < scope_limit ||
            s.related < related_limit || s.warnings < r.warnings.size()) reasons.push_back("evidence_budget");
        if (source_omissions) reasons.push_back("source_excerpt");
        if (r.warning_omissions) reasons.push_back("diagnostic_retention_cap");
        ScanStats stats = r.stats;
        stats.stop_reason = reasons.empty() ? "" : reasons.front();
        stats.rows_truncated = r.candidate_omissions + r.retention_omissions + r.memory_omissions +
            r.evidence.size() - s.rows.size() + files.size() - s.files + scopes.size() - s.scopes + related.size() - s.related;
        stats.rows_output = 2 + s.rows.size() + s.levels.size() + s.files + s.scopes + s.related + s.warnings + !r.plan_record.empty();

        std::string out = r.plan_record;
        std::string coverage;
        for (size_t cat = 0; cat < CategoryCount; ++cat) {
            std::string status = emitted[cat] ? "found" : r.found[cat] ? "omitted_by_limit"
                : cat == Dependency || cat == Test || cat == Config || cat == Documentation
                    ? (!r.followup_ran ? "not_searched_for_related_identifiers" : "no_match_in_selected_inputs")
                    : "no_match_in_selected_inputs";
            if (!scan_complete && !emitted[cat] && !r.found[cat]) status = "input_failure";
            if (llm) coverage += " " + std::string(category_name(EvidenceCategory(cat))) + "=" + status;
            else { if (cat) coverage += ','; coverage += q(category_name(EvidenceCategory(cat))) + ':' + q(status); }
        }
        std::vector<std::string> inputs(cli.positional);
        inputs.insert(inputs.end(), cli.globs.begin(), cli.globs.end());
        for (const auto &list : cli.file_lists) inputs.push_back("file-list:" + list.path);
        if (cli.git_changed || cli.git_staged || cli.git_untracked || !cli.git_ranges.empty()) inputs.push_back("git-selection");
        if (inputs.empty()) inputs.push_back("<stdin>");
        const std::string follow_status = r.followup_ran ? "searched" : r.followup_requested ? "no_candidates" : "disabled";
        if (llm) {
            out += "INVESTIGATION profile=" + r.profile + " seeds=" + strings(r.seeds) +
                " scans=" + std::to_string(r.stats.scan_stages) + " complete=" + (complete ? "yes" : "no") + "\n\nSUMMARY\n" +
                std::to_string(r.stats.matches_seen) + " seed matches in " + std::to_string(r.seed_files) +
                " files; " + std::to_string(r.seed_definitions) + " probable definitions/declarations; " +
                std::to_string(r.seed_tests) + " seed-containing test files; " + std::to_string(r.related_tests) +
                " helper-associated test files.\n" + "inputs=" + strings(inputs) + " exclusions=" + strings(cli.excludes) +
                " followup=" + follow_status + "\n";
            out += "scan_complete=" + boolean(scan_complete) + " expansion_complete=" + boolean(expansion_complete) +
                " output_complete=" + boolean(output_complete) + "\ncoverage:" + coverage + "\n";
        } else {
            out += "{\"type\":\"investigation-summary\",\"profile\":" + q(r.profile) + ",\"seeds\":" + strings(r.seeds) +
                ",\"files_with_seed\":" + std::to_string(r.seed_files) + ",\"seed_matches\":" + std::to_string(r.stats.matches_seen) +
                ",\"probable_definitions\":" + std::to_string(r.seed_definitions) +
                ",\"probable_references\":" + std::to_string(r.stats.matches_seen - r.seed_definitions) +
                ",\"tests\":" + std::to_string(r.seed_tests) + ",\"configs\":" + std::to_string(r.seed_configs) +
                ",\"related_tests\":" + std::to_string(r.related_tests) + ",\"related_matches\":" + std::to_string(r.related_matches) +
                ",\"scan_stages\":" + std::to_string(r.stats.scan_stages) + ",\"complete\":" + boolean(complete) +
                ",\"scan_complete\":" + boolean(scan_complete) + ",\"expansion_complete\":" + boolean(expansion_complete) +
                ",\"output_complete\":" + boolean(output_complete) + ",\"followup\":" + q(follow_status) +
                ",\"inputs\":" + strings(inputs) + ",\"exclusions\":" + strings(cli.excludes) + ",\"coverage\":{" + coverage + "}";
            if (!cli.file_where.empty() || !cli.relations.empty() || !cli.in_scopes.empty() ||
                !cli.in_scope_kind.empty() || !cli.line_ranges.empty() || cli.git_added_lines) {
                out += ",\"filters\":{\"seed_file_where\":" + q(cli.file_where) +
                    ",\"seed_relations\":" + std::to_string(cli.relations.size()) +
                    ",\"both_stage_scopes\":" + strings(cli.in_scopes) +
                    ",\"both_stage_scope_kind\":" + q(cli.in_scope_kind) +
                    ",\"git_added_lines\":" + boolean(cli.git_added_lines) + ",\"both_stage_lines\":[";
                for (size_t i = 0; i < cli.line_ranges.size(); ++i) {
                    if (i) out += ',';
                    out += '[' + std::to_string(cli.line_ranges[i].lo) + ',' + std::to_string(cli.line_ranges[i].hi) + ']';
                }
                out += "]}";
            }
            out += "}\n";
        }
        // Seed-pattern predicates are not reinterpreted over derived patterns.
        if (llm && (!cli.file_where.empty() || !cli.relations.empty() || !cli.in_scopes.empty() ||
                    !cli.line_ranges.empty() || cli.git_added_lines))
            out += "filters: seed-pattern predicates apply to seeds; input, line, scope and Git restrictions apply to both stages\n";
        std::set<std::string> emitted_chunks;
        if (llm && !s.rows.empty()) out += "\nPROBABLE DEFINITIONS / DECLARATIONS AND REPRESENTATIVE EVIDENCE\n";
        for (size_t i : s.rows) {
            const auto &e = r.evidence[i];
            out += evidence_record(e, chunk_ids.at(e.chunk_key), s.levels.at(e.chunk_key), llm);
            if (emitted_chunks.insert(e.chunk_key).second)
                out += render_source_chunk(chunks.at(e.chunk_key), chunk_ids.at(e.chunk_key), s.levels.at(e.chunk_key), llm);
        }
        if (llm && s.files) out += "\nTOP FILES\n";
        for (size_t i = 0; i < s.files; ++i) out += files[i];
        if (llm && s.scopes) out += "\nTOP SCOPES\n";
        for (size_t i = 0; i < s.scopes; ++i) out += scopes[i];
        if (llm && s.related) out += "\nRELATED IDENTIFIERS\n";
        for (size_t i = 0; i < s.related; ++i) out += related[i];
        for (size_t i = 0; i < s.warnings; ++i) out += r.warnings[i];
        const uint64_t omitted_evidence = r.evidence.size() - s.rows.size();
        if (llm) {
            out += "\nLIMITS\n" + std::string(complete ? "No sections truncated.\n" : "Selected lexical evidence; omissions are listed below.\n") +
                "candidate_patterns_omitted=" + std::to_string(r.candidate_omissions) +
                " retention_omitted=" + std::to_string(r.retention_omissions) +
                " memory_limit_events=" + std::to_string(r.memory_omissions) +
                " examples_omitted=" + std::to_string(omitted_evidence) + " source_excerpts=" + std::to_string(source_omissions) +
                " scope_fallbacks=" + std::to_string(r.scope_fallbacks) + "\n";
            if (!reasons.empty()) out += "omission_reasons=" + strings(reasons) + " stop_reason=" + reasons.front() + "\n";
        } else {
            out += "{\"type\":\"investigation-footer\",\"complete\":" + boolean(complete) +
                ",\"omitted_files\":" + std::to_string(file_limit - s.files) +
                ",\"omitted_scopes\":" + std::to_string(scope_limit - s.scopes) +
                ",\"omitted_related\":" + std::to_string(related_limit - s.related) +
                ",\"omitted_evidence\":" + std::to_string(anchor_limit - s.rows.size()) +
                ",\"candidate_patterns_omitted\":" + std::to_string(r.candidate_omissions) +
                ",\"top_files_limit_omitted\":" + std::to_string(files.size() - file_limit) +
                ",\"top_scopes_limit_omitted\":" + std::to_string(scopes.size() - scope_limit) +
                ",\"related_limit_omitted\":" + std::to_string(related.size() - related_limit) +
                ",\"examples_limit_omitted\":" + std::to_string(r.evidence.size() - anchor_limit) +
                ",\"retention_omitted\":" + std::to_string(r.retention_omissions) +
                ",\"memory_limit_events\":" + std::to_string(r.memory_omissions) +
                ",\"source_excerpts\":" + std::to_string(source_omissions) +
                ",\"scope_fallbacks\":" + std::to_string(r.scope_fallbacks) +
                ",\"followup_files_failed\":" + std::to_string(r.followup_failures) +
                ",\"omitted_diagnostics\":" + std::to_string(r.warnings.size() - s.warnings + r.warning_omissions) +
                ",\"omission_reasons\":" + strings(reasons);
            if (!stats.stop_reason.empty()) out += ",\"stop_reason\":" + q(stats.stop_reason);
            out += "}\n";
        }
        if (cli.summary) out += summary_record(stats, stats.rows_output, elapsed_ms);
        return std::make_pair(std::move(out), complete);
    };

    Selection selected;
    selected.warnings = r.warnings.size();
    auto fits = [&](const Selection &s) { return !budget || serialize(s).first.size() <= budget; };
    while (selected.warnings && !fits(selected)) --selected.warnings;
    if (!fits(selected)) {
        std::fprintf(stderr, "hprscript: investigation byte cap %llu is below the minimum report size (%zu bytes)\n",
                     static_cast<unsigned long long>(budget), serialize(selected).first.size());
        return 2;
    }
    for (size_t k = 0; k < anchor_limit; ++k) {
        const size_t i = order[k];
        const auto &key = r.evidence[i].chunk_key;
        for (int level = 1; level <= 3; ++level) {
            Selection trial = selected;
            trial.rows.push_back(i);
            if (!trial.levels.count(key)) trial.levels[key] = level;
            if (fits(trial)) { selected = std::move(trial); break; }
        }
    }
    // Upgrade useful representatives before spending remaining bytes on
    // rankings. Unused category capacity is immediately available to others.
    const auto upgrade_order = selected.rows;
    for (size_t i : upgrade_order) {
        const auto &key = r.evidence[i].chunk_key;
        for (int level = 0; level < selected.levels[key]; ++level) {
            Selection trial = selected;
            trial.levels[key] = level;
            if (fits(trial)) { selected = std::move(trial); break; }
        }
    }
    auto add_metadata = [&](size_t Selection::*field, size_t limit) {
        while (selected.*field < limit) {
            Selection trial = selected;
            ++(trial.*field);
            if (!fits(trial)) break;
            selected = std::move(trial);
        }
    };
    add_metadata(&Selection::files, file_limit);
    add_metadata(&Selection::scopes, scope_limit);
    add_metadata(&Selection::related, related_limit);
    auto payload = serialize(selected);
    if (budget && payload.first.size() > budget) return 2; // no partial stdout
    std::fwrite(payload.first.data(), 1, payload.first.size(), stdout);
    if (cli.require_complete && !payload.second) return 2;
    return r.stats.matches_seen ? 0 : 1;
}

} // namespace hpr
