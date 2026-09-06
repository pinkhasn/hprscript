#include "investigate.hpp"
#include "investigation_report.hpp"
#include "expand.hpp"
#include "seen.hpp"
#include <cstdlib>

#include "evidence.hpp"
#include "extract.hpp"
#include "file_io.hpp"
#include "file_role.hpp"
#include "ident.hpp"
#include "language_evidence.hpp"
#include "line_index.hpp"
#include "match_row.hpp"
#include "matcher.hpp"
#include "output.hpp"
#include "pipeline.hpp"
#include "planner.hpp"
#include "rank.hpp"
#include "scope.hpp"
#include "walker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace hpr {
namespace {

struct InvestigationFile {
    std::string path;
    std::string content;
    LineIndex lines;
    ScopeIndex scopes;
    bool scope_built = false;
    RoleIndex lexical;
    bool lexical_built = false;
    uint64_t retained_bytes = 0;
    std::vector<size_t> rows;
    FileRoleResult roles;
};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool is_identifier(const std::string &s) {
    if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_'))
        return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

std::string profile_for(const Cli &cli) {
    if (cli.investigate.profile != "auto") return cli.investigate.profile;
    std::string seed;
    bool fixed = false;
    if (cli.patterns.size() == 1) {
        const auto &p = cli.patterns.front();
        fixed = p.fixed || !p.ident_terms.empty();
        if (!p.ident_terms.empty()) {
            for (const auto &term : p.ident_terms) seed += term;
        } else seed = p.regexp;
    }
    std::string l = lower(seed);
    if (l.find("error") != std::string::npos || l.find("err") == 0 ||
        l.find("exception") != std::string::npos || l.find("panic") != std::string::npos)
        return "error";
    bool upper_snake = !seed.empty() && seed.find('_') != std::string::npos;
    for (unsigned char c : seed)
        if (std::islower(c) || (!std::isalnum(c) && c != '_')) upper_snake = false;
    if (upper_snake || l.find("config") != std::string::npos ||
        l.find(".yaml") != std::string::npos || l.find(".env") != std::string::npos)
        return "config";
    if (fixed && is_identifier(seed)) return "symbol";
    return "concept";
}

std::vector<std::string> seed_labels(const Cli &cli) {
    std::vector<std::string> out;
    for (const auto &p : cli.patterns) {
        if (!p.ident_terms.empty()) {
            std::string s;
            for (const auto &t : p.ident_terms) {
                if (!s.empty()) s += ' ';
                s += t;
            }
            out.push_back(std::move(s));
        } else out.push_back(p.regexp);
    }
    return out;
}

const std::unordered_set<std::string> &stop_words() {
    static const std::unordered_set<std::string> words = {
        "the","and","for","with","this","that","from","return","true","false",
        "null","void","const","static","public","private","class","struct","func",
        "function","import","include","string","auto","else","while","switch","case",
        "break","continue","namespace","using","std","self","super","var","let","new",
        "int","long","short","char","bool","float","double","unsigned","signed","size_t",
        "vector","map","set","true","false","nullptr","return","ifdef","endif"
    };
    return words;
}

std::string regex_escape(const std::string &s) {
    static const char *special = "\\^$.[]|()?*+{}";
    std::string out;
    for (char c : s) {
        if (std::strchr(special, c)) out += '\\';
        out += c;
    }
    return out;
}

void truncate_utf8(std::string &value, uint64_t limit) {
    if (!limit || value.size() <= limit) return;
    size_t end = static_cast<size_t>(limit);
    while (end > 0 &&
           (static_cast<unsigned char>(value[end]) & 0xc0u) == 0x80u) --end;
    value.resize(end);
}

bool has_role(const FileRoleResult &r, const std::string &role) {
    return std::find(r.roles.begin(), r.roles.end(), role) != r.roles.end();
}

} // namespace


namespace {

bool definition(const OccurrenceClassification &c) {
    return c.classification == "probable_definition" || c.classification == "probable_declaration";
}

const ScopeRange *source_scope(const ScopeIndex &scopes, uint64_t offset) {
    // Source ownership includes a return type/modifier preceding the name.
    // Keep the historical anchor offsets used by query/edit unchanged.
    const ScopeRange *best = nullptr;
    for (const auto &scope : scopes.all())
        if (scope.signature_from <= offset && offset < scope.end_off &&
            (!best || scope.end_off - scope.signature_from < best->end_off - best->signature_from)) best = &scope;
    return best;
}

EvidenceCategory category_for(const OccurrenceClassification &c, const FileRoleResult &roles, bool derived) {
    if (has_role(roles, "test") && c.classification != "comment" && c.classification != "string") return Test;
    if (has_role(roles, "config")) return Config;
    if (has_role(roles, "documentation")) return Documentation;
    if (definition(c)) return derived ? Dependency : SeedDefinition;
    if (!derived && c.classification == "probable_call_or_reference") return Caller;
    return Other;
}

uint64_t origin_bytes(const EvidenceOrigin &o) {
    return sizeof(o) + o.file.size() + o.kind.size() + 64;
}

uint64_t row_bytes(const InvestigationEvidence &e) {
    const auto &r = e.row;
    uint64_t bytes = sizeof(e) + 256 + r.file.size() + r.language.size() + r.match.size() +
        r.context.size() + r.set_id.size() + r.pattern_id.size() + r.derived_value.size() +
        r.derived_source_rows.size() * sizeof(uint64_t) + e.chunk_key.size() + e.ref.size();
    if (r.enclosing) bytes += sizeof(ScopeRef) + r.enclosing->name.size() + r.enclosing->kind.size();
    for (const auto &o : e.origins) bytes += origin_bytes(o);
    for (const auto &role : e.file_roles) bytes += sizeof(std::string) + role.size();
    for (const auto &[key, value] : r.captures) bytes += 128 + key.size() + value.to_str().size();
    return bytes;
}

} // namespace

int run_investigate(const Cli &cli) {
    if (cli.context_before < 0 || cli.context_after < 0) {
        std::fprintf(stderr, "hprscript: investigation context must be nonnegative\n");
        return 2;
    }
    const auto started = std::chrono::steady_clock::now();
    InvestigationReport report;
    report.profile = profile_for(cli);
    report.seeds = seed_labels(cli);
    report.followup_requested = cli.investigate.followup != InvestigateOptions::Followup::Never;
    auto &stats = report.stats;
    EvidenceMemory memory{cli.investigate.max_memory_bytes};
    std::vector<Pattern> patterns = build_patterns(cli);
    const std::string scope_lang = cli.scope_lang.empty() && cli.scope_pattern.empty() ? "auto" : cli.scope_lang;
    ScopeConfig custom{cli.scope_pattern, cli.scope_open, cli.scope_close, cli.scope_kind, {}};
    if (!scope_lang.empty() && scope_lang != "auto" && !builtin_scope_pack(scope_lang)) {
        std::fprintf(stderr, "hprscript: unknown -scope pack '%s'\n", scope_lang.c_str());
        return 2;
    }
    uint64_t evidence_budget = cli.investigate.evidence_budget;
    if (cli.max_output_bytes && (!evidence_budget || cli.max_output_bytes < evidence_budget))
        evidence_budget = cli.max_output_bytes;
    if (cli.explain_plan) {
        ExecutionPlan plan;
        plan.mode = "investigate";
        PlanStage seed;
        seed.id = "seed-scan"; seed.sets = {"seeds"}; seed.patterns = patterns.size();
        seed.inputs = cli.positional;
        seed.inputs.insert(seed.inputs.end(), cli.globs.begin(), cli.globs.end());
        for (const auto &list : cli.file_lists) seed.inputs.push_back("file-list:" + list.path);
        if (cli.git_changed || cli.git_staged || cli.git_untracked || !cli.git_ranges.empty())
            seed.inputs.push_back("git-selection");
        if (seed.inputs.empty()) seed.inputs.push_back("<stdin>");
        seed.scope = scope_lang.empty() ? "custom" : scope_lang;
        plan.scan_stages.push_back(seed);
        if (report.followup_requested) {
            PlanStage follow = seed;
            follow.id = "related-followup"; follow.sets = {"derived-related-identifiers"};
            follow.patterns = cli.investigate.max_related_patterns; follow.adaptive = true;
            plan.scan_stages.push_back(follow);
        }
        plan.postprocess = {{"classify-and-derive-origins", {}}, {"retain-related-source", {}},
                            {"select-category-evidence", {}}, {"pack-complete-payload", {}}};
        plan.limits = {{"top_files", cli.investigate.top_files}, {"top_scopes", cli.investigate.top_scopes},
                       {"related", cli.investigate.related}, {"examples", cli.investigate.examples},
                       {"evidence_bytes", evidence_budget}, {"max_memory_bytes", memory.limit}};
        report.plan_record = execution_plan_record(plan);
        if (cli.plan_only) {
            if (evidence_budget && report.plan_record.size() > evidence_budget) {
                std::fprintf(stderr, "hprscript: investigation byte cap is below the minimum plan size\n");
                return 2;
            }
            std::fwrite(report.plan_record.data(), 1, report.plan_record.size(), stdout);
            return 0;
        }
    }
    auto warning = [&](const char *code, const std::string &path) {
        if (!cli.diagnostics) {
            std::fprintf(stderr, "hprscript: %s: %s\n", code, path.c_str());
            return;
        }
        std::string value = "{\"type\":\"warning\",\"code\":\"" + json_escape(code) +
                "\",\"file\":\"" + json_escape(path) + "\"}\n";
        if (memory.reserve(value.size() + 64)) report.warnings.push_back(std::move(value));
        else ++report.warning_omissions;
    };
    std::vector<ResolvedRelation> relations;
    if (!resolve_relations(cli.relations, patterns, relations)) return 2;
    FileWhere fw;
    if (!fw.init(cli.file_where, patterns)) return 2;
    std::map<int, std::unordered_map<std::string, uint32_t>> churn;
    if (!fw.churn_windows().empty()) {
        std::string err;
        if (!build_churn_map(fw.churn_windows(), churn, err)) {
            std::fprintf(stderr, "hprscript: git: %s\n", err.c_str()); return 2;
        }
    }
    TargetFilter target;
    if (!target.init(cli)) return 2;
    std::vector<IdentGroup> ident_groups;
    for (const auto &p : cli.patterns)
        if (!p.ident_terms.empty()) ident_groups.push_back({p.ident_terms});
    const size_t regex_count = patterns.size() - ident_groups.size();
    Matcher matcher;
    if (regex_count) {
        CompileError error;
        std::vector<Pattern> regex(patterns.begin(), patterns.begin() + regex_count);
        if (!matcher.compile(regex, &error)) {
            std::fprintf(stderr, "hprscript: pattern compile failed: %s\n", error.message.c_str()); return 2;
        }
        ++stats.matcher_compilations;
    }
    stats.patterns_compiled += patterns.size();
    ExtractTable extracts;
    std::string extract_error; int extract_index = -1;
    if (!extracts.build(patterns, &extract_error, &extract_index)) {
        std::fprintf(stderr, "hprscript: %s\n", extract_error.c_str()); return 2;
    }
    MatchCollector seed_collector(patterns, std::move(relations), cli.git_added_lines, ident_groups);

    Walker walker;
    std::unordered_map<std::string, AddedLines> added;
    if (!add_walker_inputs(cli, walker, stats, added, warning)) return 2;
    const bool no_inputs = cli.globs.empty() && cli.positional.empty() && cli.file_lists.empty() &&
        !cli.git_changed && !cli.git_staged && !cli.git_untracked && cli.git_ranges.empty();
    const bool from_stdin = no_inputs && !isatty(fileno(stdin));
    std::string stdin_content;
    if (from_stdin && !read_stdin(stdin_content)) {
        std::fprintf(stderr, "hprscript: failed to read stdin\n"); return 2;
    }
    // Resolve once, then reuse precisely this corpus. Canonical ordering also
    // makes bounded retention independent of argv/files-from traversal order.
    std::set<std::string> input_paths;
    if (!from_stdin) walker.walk([&](const WalkItem &it) { input_paths.insert(it.path); return true; },
                                [&](const std::string &path) { ++stats.files_failed; warning("traversal_error", path); });

    RankInput ranking;
    ranking.total_queried = patterns.size(); ranking.surprise = true; ranking.rich_clusters = true;
    for (const auto &p : patterns) {
        ranking.queried_ids.insert(p.id); ranking.pattern_weights[p.id] = p.weight;
    }
    std::deque<InvestigationFile> seed_files;
    std::map<std::tuple<std::string, uint64_t, uint64_t>, size_t> locations;
    std::map<std::tuple<size_t, EvidenceCategory, bool>, size_t> retained_per_category;
    const size_t representatives = std::max(2, cli.investigate.examples);
    uint64_t next_row = 1;

    auto retain = [&](InvestigationEvidence e, const std::string &path, const LineIndex &idx,
                      const ScopeRange *scope, size_t bucket) -> size_t {
        const auto location = std::make_tuple(path, e.row.from, e.row.to);
        if (auto it = locations.find(location); it != locations.end()) {
            auto &prior = report.evidence[it->second];
            for (const auto &o : e.origins) {
                auto same = [&](const EvidenceOrigin &old) {
                    return old.seed == o.seed && old.file == o.file && old.line == o.line && old.kind == o.kind;
                };
                if (std::any_of(prior.origins.begin(), prior.origins.end(), same)) continue;
                if (!memory.reserve(origin_bytes(o) + sizeof(uint64_t))) { ++report.memory_omissions; break; }
                prior.origins.push_back(o);
                if (e.derived) prior.row.derived_source_rows.push_back(o.source_row);
            }
            if (prior.row.derived_value.empty()) prior.row.derived_value = e.row.derived_value;
            return it->second;
        }
        ++report.found[e.category];
        // Reserve a distinct pool for bodies: earlier prototypes/references
        // cannot crowd out a definition found later in the corpus.
        auto &count = retained_per_category[{bucket, e.category,
                                             e.classification.classification == "probable_definition"}];
        if (count >= representatives) { ++report.retention_omissions; return SIZE_MAX; }
        e.chunk_key = path + (scope ? "\nS" + std::to_string(scope->signature_from)
                                   : "\nW" + std::to_string(e.row.line));
        if (!scope) {
            const uint32_t before = cli.context_set ? cli.context_before : 2;
            const uint32_t after = cli.context_set ? cli.context_after : 2;
            const uint32_t lo = e.row.line > before ? e.row.line - before : 1;
            const uint64_t hi = uint64_t(e.row.line) + after;
            for (const auto &[key, chunk] : report.chunks)
                if (chunk.file == path && !chunk.signature_first && chunk.first <= hi && chunk.last >= lo) {
                    e.chunk_key = key; break;
                }
        }
        const uint64_t bytes = row_bytes(e);
        if (!memory.reserve(bytes)) { ++report.memory_omissions; return SIZE_MAX; }
        auto old = report.chunks.find(e.chunk_key);
        const uint64_t old_bytes = old == report.chunks.end() ? 0 : old->second.memory_bytes();
        uint64_t available = memory.limit ? memory.remaining() + old_bytes : 0;
        if (memory.limit && available < sizeof(SourceChunk) + path.size() + 160) {
            memory.release(bytes); ++report.memory_omissions; return SIZE_MAX;
        }
        SourceChunk chunk = make_source_chunk(path, idx, scope, e.row.line, cli, available);
        if (old != report.chunks.end()) merge_source_chunk(chunk, old->second);
        const uint64_t new_bytes = chunk.memory_bytes();
        if (new_bytes > old_bytes && !memory.reserve(new_bytes - old_bytes)) {
            memory.release(bytes); ++report.memory_omissions; return SIZE_MAX;
        }
        if (old_bytes > new_bytes) memory.release(old_bytes - new_bytes);
        if (chunk.retention_reduced) ++report.memory_omissions;
        if (!chunk.reliable_scope) ++report.scope_fallbacks;
        report.chunks[e.chunk_key] = std::move(chunk);
        ++count;
        const size_t index = report.evidence.size();
        locations[location] = index;
        report.evidence.push_back(std::move(e));
        ++stats.rows_materialized;
        return index;
    };

    auto scan_seed = [&](const std::string &path, std::string_view content) {
        LineIndex idx; idx.build(content);
        ScopeIndex scope_index;
        const auto *scope = build_file_scope(scope_lang, custom, path, content, idx, scope_index);
        RoleIndex roles;
        const auto *role_cfg = role_config_for_path(path);
        if (role_cfg) roles.build(content, *role_cfg, idx);
        std::vector<Match> kept;
        const auto added_it = added.find(path);
        const AddedLines *lines = added_it == added.end() ? nullptr : &added_it->second;
        seed_collector.collect(matcher, content, idx, scope, lines, kept);
        target.apply(kept, idx, scope);
        if (fw.active() && !fw.pass(kept, patterns.size(), path, churn)) return;
        stats.matches_seen += kept.size();
        if (kept.empty()) return;
        ++report.seed_files;
        FileRoleResult file_roles = classify_file_roles(path);
        if (has_role(file_roles, "test")) ++report.seed_tests;
        if (has_role(file_roles, "config")) ++report.seed_configs;
        const uint64_t rank_bytes = path.size() + 512 + std::min<size_t>(4096, kept.size()) * 32 +
                                    patterns.size() * 64;
        if (memory.reserve(rank_bytes)) accumulate_rank_input(ranking, path, kept, patterns, idx);
        else ++report.memory_omissions;
        const uint64_t minimum_file_bytes = sizeof(InvestigationFile) + path.size() + content.size() +
            idx.memory_bytes() + scope_index.memory_bytes() + roles.memory_bytes();
        if (memory.limit && minimum_file_bytes > memory.remaining()) {
            for (const auto &m : kept) {
                auto c = classify_occurrence(path, content, m.from, m.to, idx, scope, role_cfg ? &roles : nullptr);
                if (definition(c)) ++report.seed_definitions;
                ++report.found[category_for(c, file_roles, false)];
                ++report.memory_omissions;
            }
            return;
        }
        seed_files.emplace_back();
        auto &f = seed_files.back();
        f.path = path; f.content.assign(content);
        f.lines.build(f.content);
        f.scopes = std::move(scope_index); f.scope_built = scope != nullptr;
        f.lexical = std::move(roles); f.lexical_built = role_cfg != nullptr;
        f.roles = file_roles;
        f.retained_bytes = sizeof(f) + f.path.size() + f.content.capacity() + f.lines.memory_bytes() +
                           f.scopes.memory_bytes() + f.lexical.memory_bytes();
        const bool retained_file = memory.reserve(f.retained_bytes);
        // Definitions are counted even when their source cannot be retained.
        for (const auto &m : kept) {
            auto c = classify_occurrence(path, content, m.from, m.to, idx,
                                         f.scope_built ? &f.scopes : nullptr, role_cfg ? &f.lexical : nullptr);
            if (definition(c)) ++report.seed_definitions;
            if (!retained_file) { ++report.found[category_for(c, file_roles, false)]; ++report.memory_omissions; continue; }
            InvestigationEvidence e;
            e.row = materialize_match_row(next_row++, 0, "seeds", patterns, m, path, content,
                                          idx, f.scope_built ? &f.scopes : nullptr, extracts.any() ? &extracts : nullptr);
            truncate_utf8(e.row.match, cli.max_match_bytes);
            truncate_utf8(e.row.context, cli.max_context_bytes);
            e.classification = std::move(c); e.file_roles = file_roles.roles;
            e.category = category_for(e.classification, file_roles, false);
            e.origins.push_back({e.row.row_id, m.pattern_index, path, e.row.line,
                                e.row.enclosing ? e.row.enclosing->from : m.from,
                                definition(e.classification) ? "seed_definition" : "seed_occurrence", 0});
            if (cli.refs && path != "<stdin>") e.ref = path + ":" + std::to_string(e.row.line) + "@" + ref_hash6(idx.line_text(e.row.line));
            const auto *enclosing = f.scope_built ? source_scope(f.scopes, m.from) : nullptr;
            const size_t row = retain(std::move(e), path, idx, enclosing, m.pattern_index);
            if (row != SIZE_MAX && std::find(f.rows.begin(), f.rows.end(), row) == f.rows.end()) {
                if (memory.reserve(sizeof(size_t))) { f.rows.push_back(row); f.retained_bytes += sizeof(size_t); }
                else ++report.memory_omissions;
            }
        }
        if (!retained_file) seed_files.pop_back();
        else if (f.rows.empty()) { memory.release(f.retained_bytes); seed_files.pop_back(); }
    };
    ++stats.scan_stages;
    if (from_stdin) {
        ++stats.files_scanned; stats.bytes_scanned += stdin_content.size();
        scan_seed("<stdin>", stdin_content);
    } else {
        for (const auto &path : input_paths) {
            MappedFile file;
            if (!file.open(path)) { ++stats.files_failed; warning("read_error", path); continue; }
            if (looks_binary(file.view())) { ++stats.files_binary; continue; }
            ++stats.files_scanned; stats.bytes_scanned += file.view().size();
            scan_seed(path, file.view());
        }
    }

    std::set<std::string> seed_names;
    for (const auto &seed : report.seeds) seed_names.insert(lower(seed));
    std::map<std::string, InvestigationCandidate> candidates;
    const std::set<std::string> generic = {"cli", "stats", "size", "empty", "doc", "data", "begin", "end", "value",
        "build", "get", "set", "clear", "push_back", "insert", "find", "c_str", "front", "back"};
    const std::set<std::string> keywords = {"if", "try", "catch", "throw", "sizeof", "switch", "do", "while",
        "for", "namespace", "package", "const", "let", "var", "function", "func", "class", "struct", "enum",
        "interface", "type", "fn", "pub", "mut", "impl", "use", "in", "of", "as"};
    for (const auto &f : seed_files) {
        for (size_t row_index : f.rows) {
            const auto &e = report.evidence[row_index];
            const auto &row = e.row;
            const auto *scope = f.scope_built ? source_scope(f.scopes, row.from) : nullptr;
            const bool seed_definition = e.classification.classification == "probable_definition" && scope &&
                                         f.scopes.declared_at(row.from, row.to);
            const bool string_profile = report.profile == "error" || report.profile == "config";
            if (!string_profile && f.lexical_built && f.lexical.at(row.from) != LexRole::Code) continue;
            uint64_t lo = 0, hi = 0;
            if (seed_definition) { lo = scope->signature_from; hi = scope->end_off; }
            else {
                auto first = f.lines.line_text(row.line > 2 ? row.line - 2 : 1);
                lo = first.data() - f.content.data();
                auto last = f.lines.line_text(std::min(f.lines.line_count(), row.line + 2));
                hi = last.data() - f.content.data() + last.size();
            }
            std::vector<IdentifierToken> token_spans;
            scan_identifier_tokens(std::string_view(f.content).substr(lo, hi - lo), token_spans);
            for (const auto &token : token_spans) {
                const uint64_t from = lo + token.from, to = lo + token.to;
                if (to - from > 80 || (from <= row.from && to >= row.to)) continue;
                const std::string name = f.content.substr(from, to - from), normalized = lower(name);
                if (seed_names.count(normalized) || stop_words().count(normalized) || keywords.count(normalized)) continue;
                if (!string_profile && f.lexical_built && f.lexical.at(from) != LexRole::Code) continue;
                uint64_t after = to;
                while (after < hi && std::isspace(static_cast<unsigned char>(f.content[after]))) ++after;
                const bool call = after < hi && f.content[after] == '(' &&
                                  (!f.lexical_built || f.lexical.at(from) == LexRole::Code);
                const bool named_type = !name.empty() && std::isupper(static_cast<unsigned char>(name[0]));
                int priority = seed_definition ? (call ? (generic.count(name) ? 1 : 0) : named_type ? 1 : 2) : 3;
                const std::string association = seed_definition ? (call ? "called_in_seed_definition" : "used_in_seed_definition")
                                                               : "near_seed_reference";
                auto found = candidates.find(name);
                if (found == candidates.end()) {
                    if (!memory.reserve(sizeof(InvestigationCandidate) + name.size() + 96)) { ++report.memory_omissions; continue; }
                    found = candidates.emplace(name, InvestigationCandidate{}).first;
                    found->second.identifier = name;
                }
                auto &c = found->second;
                c.priority = std::min(c.priority, priority);
                for (const auto &seed_origin : e.origins) {
                    EvidenceOrigin origin{row.row_id, seed_origin.seed, f.path, f.lines.line_of(from),
                                          scope ? scope->start_off : row.from, association, priority};
                    auto same = [&](const EvidenceOrigin &o) {
                        return o.seed == origin.seed && o.file == origin.file && o.scope_from == origin.scope_from && o.kind == origin.kind;
                    };
                    if (std::any_of(c.origins.begin(), c.origins.end(), same)) continue;
                    if (!memory.reserve(origin_bytes(origin))) { ++report.memory_omissions; continue; }
                    c.origins.push_back(std::move(origin));
                    if (seed_definition) ++c.same_scope_hits;
                    else ++c.same_window_hits;
                }
            }
        }
    }
    // Discovery is complete before aggregation. No candidate misses counts
    // just because its first origin happened to occur in a later input file.
    for (const auto &f : seed_files) {
        std::set<std::string> seen;
        std::vector<IdentifierToken> spans; scan_identifier_tokens(f.content, spans);
        for (const auto &span : spans) {
            if (f.lexical_built && f.lexical.at(span.from) != LexRole::Code &&
                report.profile != "error" && report.profile != "config") continue;
            const std::string id = f.content.substr(span.from, span.to - span.from);
            if (auto it = candidates.find(id); it != candidates.end()) {
                ++it->second.same_file_hits;
                seen.insert(id);
            }
        }
        for (const auto &id : seen) ++candidates[id].seed_files;
    }
    std::vector<InvestigationCandidate *> ranked;
    for (auto &[id, c] : candidates) {
        if (c.origins.empty()) continue;
        c.score = c.same_scope_hits * 4.0 + c.same_window_hits * 2.0 + c.seed_files;
        ranked.push_back(&c);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto *a, const auto *b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        if (a->score != b->score) return a->score > b->score;
        return a->identifier < b->identifier;
    });
    const size_t cap = cli.investigate.max_related_patterns;
    std::set<std::string> chosen;
    // Reserve candidates for quieter seeds as well as the most frequent seed.
    for (size_t round = 0; chosen.size() < cap; ++round) {
        bool progress = false;
        for (size_t seed = 0; seed < patterns.size() && chosen.size() < cap; ++seed) {
            for (auto *c : ranked) {
                if (chosen.count(c->identifier)) continue;
                if (std::none_of(c->origins.begin(), c->origins.end(), [&](const auto &o) { return o.seed == seed; })) continue;
                chosen.insert(c->identifier); progress = true; break;
            }
        }
        if (!progress) break;
    }
    report.candidate_omissions = ranked.size() - chosen.size();
    for (auto *c : ranked)
        if (chosen.count(c->identifier)) report.candidates.push_back(std::move(*c));
    // Release full seed files after candidate extraction; chunks own every
    // source byte needed for rendering. Follow-up retention shares this budget.
    for (const auto &f : seed_files) memory.release(f.retained_bytes);
    seed_files.clear();
    for (const auto &[id, c] : candidates)
        if (!chosen.count(id)) {
            uint64_t bytes = sizeof(c) + id.size() + 96;
            for (const auto &o : c.origins) bytes += origin_bytes(o);
            memory.release(bytes);
        }
    candidates.clear();

    uint64_t corpus_files = 0;
    if (report.followup_requested && !report.candidates.empty()) {
        report.followup_ran = true;
        std::vector<Pattern> derived;
        for (size_t i = 0; i < report.candidates.size(); ++i) {
            Pattern p; p.id = "related" + std::to_string(i);
            p.regexp = regex_escape(report.candidates[i].identifier); p.word_boundary = true;
            derived.push_back(std::move(p));
        }
        Matcher related_matcher; CompileError error;
        if (!related_matcher.compile(derived, &error)) {
            std::fprintf(stderr, "hprscript: related matcher compile failed: %s\n", error.message.c_str()); return 2;
        }
        ++stats.matcher_compilations; stats.patterns_compiled += derived.size(); ++stats.scan_stages;
        MatchCollector related_collector(derived, {}, cli.git_added_lines);
        auto scan_related = [&](const std::string &path, std::string_view content) {
            ++corpus_files;
            LineIndex idx; idx.build(content);
            ScopeIndex scopes;
            const auto *scope = build_file_scope(scope_lang, custom, path, content, idx, scopes);
            RoleIndex roles; const auto *role_cfg = role_config_for_path(path);
            if (role_cfg) roles.build(content, *role_cfg, idx);
            const auto file_roles = classify_file_roles(path);
            const auto added_it = added.find(path);
            const AddedLines *lines = added_it == added.end() ? nullptr : &added_it->second;
            std::vector<Match> kept;
            related_collector.collect(related_matcher, content, idx, scope, lines, kept);
            target.apply(kept, idx, scope);
            report.related_matches += kept.size();
            std::vector<bool> hit(derived.size(), false);
            bool associated_test = false;
            for (const auto &m : kept) {
                auto &c = report.candidates[m.pattern_index];
                hit[m.pattern_index] = true;
                auto classification = classify_occurrence(path, content, m.from, m.to, idx, scope, role_cfg ? &roles : nullptr);
                if (classification.classification == "probable_definition") ++c.definitions;
                if (has_role(file_roles, "test") && classification.classification != "comment" &&
                    classification.classification != "string") associated_test = true;
                InvestigationEvidence e;
                e.row = materialize_match_row(next_row++, 1, "related", derived, m, path, content, idx, scope);
                truncate_utf8(e.row.match, cli.max_match_bytes);
                truncate_utf8(e.row.context, cli.max_context_bytes);
                e.row.derived_value = c.identifier;
                for (const auto &o : c.origins) e.row.derived_source_rows.push_back(o.source_row);
                std::sort(e.row.derived_source_rows.begin(), e.row.derived_source_rows.end());
                e.row.derived_source_rows.erase(std::unique(e.row.derived_source_rows.begin(), e.row.derived_source_rows.end()), e.row.derived_source_rows.end());
                e.classification = std::move(classification); e.file_roles = file_roles.roles;
                e.category = category_for(e.classification, file_roles, true);
                e.origins = c.origins; e.derived = true;
                if (cli.refs && path != "<stdin>") e.ref = path + ":" + std::to_string(e.row.line) + "@" + ref_hash6(idx.line_text(e.row.line));
                retain(std::move(e), path, idx, scope ? source_scope(scopes, m.from) : nullptr, patterns.size() + m.pattern_index);
            }
            if (associated_test) ++report.related_tests;
            for (size_t i = 0; i < hit.size(); ++i) if (hit[i]) ++report.candidates[i].corpus_files;
        };
        if (from_stdin) {
            ++stats.files_scanned; stats.bytes_scanned += stdin_content.size();
            scan_related("<stdin>", stdin_content);
        } else {
            // Like edit/apply's deterministic fault fixtures, this hook is
            // inert unless fault injection is explicitly enabled.
            const char *enable = std::getenv("HPRSCRIPT_ENABLE_FAULT_INJECTION");
            const char *fail_n = std::getenv("HPRSCRIPT_TEST_FAIL_FOLLOWUP_READ_N");
            const uint64_t fail_at = enable && std::string(enable) == "1" && fail_n ? std::strtoull(fail_n, nullptr, 10) : 0;
            uint64_t reads = 0;
            for (const auto &path : input_paths) {
                MappedFile file;
                if (++reads == fail_at || !file.open(path)) {
                    ++stats.files_failed; ++report.followup_failures; warning("followup_read_error", path); continue;
                }
                if (looks_binary(file.view())) { ++stats.files_binary; continue; }
                ++stats.files_scanned; stats.bytes_scanned += file.view().size();
                scan_related(path, file.view());
            }
        }
    }
    for (auto &c : report.candidates) {
        if (report.followup_ran)
            c.score *= std::log((double(corpus_files) + 1) / (double(c.corpus_files) + 1)) + 1;
        else c.corpus_files = c.seed_files;
    }
    for (auto &e : report.evidence) {
        for (const auto &c : report.candidates)
            if (e.row.derived_value == c.identifier && c.definitions > 1) { e.ambiguous = true; break; }
    }
    report.ranked_files = rank_files(ranking);
    for (auto &row : report.ranked_files) {
        const auto roles = classify_file_roles(row.file);
        if (report.profile == "config") {
            if (has_role(roles, "config")) row.score += 1.0;
            if (has_role(roles, "documentation")) row.score += 0.25;
        } else if (report.profile == "symbol") {
            if (has_role(roles, "source")) row.score += 0.25;
            if (has_role(roles, "test")) row.score += 0.35;
        } else if (report.profile == "error") {
            if (has_role(roles, "test")) row.score += 0.4;
            if (has_role(roles, "source")) row.score += 0.2;
        } else if (report.profile == "concept") {
            if (has_role(roles, "documentation")) row.score += 0.3;
            if (has_role(roles, "test")) row.score += 0.2;
        }
    }
    std::sort(report.ranked_files.begin(), report.ranked_files.end(), [](const auto &a, const auto &b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.density != b.density) return a.density > b.density;
        return a.file < b.file;
    });
    stats.buffered_bytes_peak = memory.peak;
    const uint64_t elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    return emit_investigation_report(cli, report, elapsed);
}

} // namespace hpr
