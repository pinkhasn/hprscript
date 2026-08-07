#include "investigate.hpp"

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
    std::vector<Match> matches;
    std::vector<MatchRow> rows;
    FileRoleResult roles;
};

struct Related {
    std::string id;
    uint64_t same_scope_hits = 0;
    uint64_t same_window_hits = 0;
    uint64_t same_file_hits = 0;
    std::set<std::string> seed_files;
    std::set<std::string> corpus_files;
    double score = 0.0;
};

struct ScopeEvidence {
    std::string file;
    ScopeRef scope;
    uint64_t matches = 0;
    double score = 0.0;
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

void tokens(std::string_view text, std::vector<std::string> &out) {
    std::vector<IdentifierToken> spans;
    scan_identifier_tokens(text, spans);
    for (const auto &span : spans) {
        size_t n = span.to - span.from;
        if (n < 3 || n > 80) continue;
        std::string value(text.substr(span.from, n));
        if (stop_words().count(lower(value))) continue;
        out.push_back(std::move(value));
    }
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

std::string json_array(const std::vector<std::string> &v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += '"'; json_escape_to(out, v[i]); out += '"';
    }
    out += ']';
    return out;
}

bool has_role(const FileRoleResult &r, const std::string &role) {
    return std::find(r.roles.begin(), r.roles.end(), role) != r.roles.end();
}

} // namespace

int run_investigate(const Cli &cli) {
    const auto started = std::chrono::steady_clock::now();
    ScanStats stats;
    std::vector<Pattern> patterns = build_patterns(cli);
    const std::string profile = profile_for(cli);
    const std::vector<std::string> seeds = seed_labels(cli);
    uint64_t evidence_budget = cli.investigate.evidence_budget;
    if (cli.max_output_bytes &&
        (!evidence_budget || cli.max_output_bytes < evidence_budget))
        evidence_budget = cli.max_output_bytes;

    std::vector<IdentGroup> ident_groups;
    for (const auto &cp : cli.patterns)
        if (!cp.ident_terms.empty()) ident_groups.push_back({cp.ident_terms});
    const size_t regex_count = patterns.size() - ident_groups.size();

    if (cli.explain_plan) {
        ExecutionPlan plan;
        plan.mode = "investigate";
        PlanStage seed;
        seed.id = "seed-scan";
        seed.sets = {"seeds"};
        seed.patterns = patterns.size();
        seed.inputs.insert(seed.inputs.end(), cli.globs.begin(), cli.globs.end());
        seed.inputs.insert(seed.inputs.end(), cli.positional.begin(), cli.positional.end());
        if (seed.inputs.empty()) seed.inputs.push_back("<stdin>");
        seed.scope = !cli.scope_lang.empty() ? cli.scope_lang
                     : (!cli.scope_pattern.empty() ? "custom" : "auto");
        plan.scan_stages.push_back(seed);
        if (cli.investigate.followup != InvestigateOptions::Followup::Never) {
            PlanStage follow;
            follow.id = "related-followup";
            follow.sets = {"derived-related-identifiers"};
            follow.patterns = static_cast<uint64_t>(cli.investigate.max_related_patterns);
            follow.inputs = seed.inputs;
            follow.adaptive = true;
            plan.scan_stages.push_back(std::move(follow));
        }
        plan.postprocess.push_back({"local-evidence", {}});
        plan.postprocess.push_back({"rank-files-scopes-related", {}});
        plan.postprocess.push_back({"pack-evidence", {}});
        plan.limits["top_files"] = cli.investigate.top_files;
        plan.limits["top_scopes"] = cli.investigate.top_scopes;
        plan.limits["related"] = cli.investigate.related;
        plan.limits["examples"] = cli.investigate.examples;
        plan.limits["evidence_bytes"] = evidence_budget;
        plan.limits["max_memory_bytes"] = cli.investigate.max_memory_bytes;
        emit_execution_plan(plan);
        if (cli.plan_only) return 0;
    }

    std::vector<ResolvedRelation> rels;
    if (!resolve_relations(cli.relations, patterns, rels)) return 2;
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

    Matcher matcher;
    if (regex_count) {
        CompileError ce;
        std::vector<Pattern> regex(patterns.begin(), patterns.begin() + regex_count);
        if (!matcher.compile(regex, &ce)) {
            std::fprintf(stderr, "hprscript: pattern compile failed: %s\n", ce.message.c_str());
            return 2;
        }
        ++stats.matcher_compilations;
    }
    stats.patterns_compiled += patterns.size();
    ExtractTable extracts;
    std::string xerr; int xidx = -1;
    if (!extracts.build(patterns, &xerr, &xidx)) {
        std::fprintf(stderr, "hprscript: %s\n", xerr.c_str()); return 2;
    }

    Walker walker;
    std::unordered_map<std::string, AddedLines> added;
    if (!add_walker_inputs(cli, walker, stats, added)) return 2;
    const bool no_inputs = cli.globs.empty() && cli.positional.empty() &&
        cli.file_lists.empty() && !cli.git_changed && !cli.git_staged &&
        !cli.git_untracked && cli.git_ranges.empty();
    const bool from_stdin = no_inputs && !isatty(fileno(stdin));
    std::string stdin_content;
    if (from_stdin && !read_stdin(stdin_content)) {
        std::fprintf(stderr, "hprscript: failed to read stdin\n"); return 2;
    }

    ScopeConfig custom{cli.scope_pattern, cli.scope_open, cli.scope_close,
                       cli.scope_kind, {}};
    std::string scope_lang = cli.scope_lang;
    if (scope_lang.empty() && cli.scope_pattern.empty()) scope_lang = "auto";
    if (!scope_lang.empty() && scope_lang != "auto" &&
        !builtin_scope_pack(scope_lang)) {
        std::fprintf(stderr, "hprscript: unknown -scope pack '%s'\n",
                     scope_lang.c_str());
        return 2;
    }
    MatchCollector collector(patterns, std::move(rels), cli.git_added_lines,
                             ident_groups);
    RankInput ranking;
    ranking.total_queried = patterns.size();
    ranking.surprise = true;
    ranking.rich_clusters = true;
    for (const auto &p : patterns) {
        ranking.queried_ids.insert(p.id);
        ranking.pattern_weights[p.id] = p.weight;
    }

    std::deque<InvestigationFile> files;
    uint64_t next_row = 1;
    uint64_t buffered = 0;
    ++stats.scan_stages;
    auto scan_seed = [&](const std::string &path, std::string_view content) -> bool {
        LineIndex idx; idx.build(content);
        ScopeIndex scopes;
        const ScopeIndex *scope = build_file_scope(scope_lang, custom, path,
                                                   content, idx, scopes);
        std::vector<Match> kept;
        const AddedLines *lines = nullptr;
        if (auto it = added.find(path); it != added.end()) lines = &it->second;
        collector.collect(matcher, content, idx, scope, lines, kept);
        target.apply(kept, idx, scope);
        stats.matches_seen += kept.size();
        if (fw.active() && !fw.pass(kept, patterns.size(), path, churn)) return true;
        if (kept.empty()) return true;
        accumulate_rank_input(ranking, path, kept, patterns, idx);
        if (cli.investigate.max_memory_bytes &&
            buffered + content.size() > cli.investigate.max_memory_bytes) {
            stats.rows_truncated += kept.size();
            if (stats.stop_reason.empty()) stats.stop_reason = "memory_cap";
            return true;
        }
        stats.rows_materialized += kept.size();
        files.emplace_back();
        InvestigationFile &f = files.back();
        f.path = path;
        f.content.assign(content.data(), content.size());
        f.lines.build(f.content);
        f.roles = classify_file_roles(path);
        ScopeConfig cfg = resolve_scope_for_file(scope_lang, custom, path);
        if (!cfg.anchor_regex.empty()) {
            std::string err;
            f.scope_built = f.scopes.build(f.content, cfg, f.lines, &err);
        }
        f.matches = kept;
        for (const auto &m : kept) {
            MatchRow row = materialize_match_row(
                next_row++, 0, "seeds", patterns, m, path, f.content, f.lines,
                f.scope_built ? &f.scopes : nullptr,
                extracts.any() ? &extracts : nullptr);
            truncate_utf8(row.match, cli.max_match_bytes);
            truncate_utf8(row.context, cli.max_context_bytes);
            f.rows.push_back(std::move(row));
        }
        buffered += content.size();
        stats.buffered_bytes_peak = std::max(stats.buffered_bytes_peak, buffered);
        return true;
    };

    if (from_stdin) {
        ++stats.files_scanned; stats.bytes_scanned += stdin_content.size();
        scan_seed("<stdin>", stdin_content);
    } else {
        walker.walk([&](const WalkItem &it) {
            MappedFile mf;
            if (!mf.open(it.path)) {
                ++stats.files_failed;
                if (cli.diagnostics) emit_warning_record("read_error", it.path);
                else std::fprintf(stderr, "hprscript: cannot read %s\n", it.path.c_str());
                return true;
            }
            if (looks_binary(mf.view())) { ++stats.files_binary; return true; }
            ++stats.files_scanned; stats.bytes_scanned += mf.view().size();
            return scan_seed(it.path, mf.view());
        });
    }

    std::unordered_set<std::string> normalized_seeds;
    for (const auto &s : seeds) normalized_seeds.insert(lower(s));
    std::map<std::string, Related> related;
    std::map<std::string, ScopeEvidence> scopes;
    for (const auto &f : files) {
        std::unordered_map<std::string, uint64_t> file_counts;
        std::vector<std::string> all;
        tokens(f.content, all);
        for (const auto &t : all) ++file_counts[t];

        std::set<uint64_t> seen_scopes;
        for (const auto &row : f.rows) {
            const uint32_t lo = row.line > 2 ? row.line - 2 : 1;
            const uint32_t hi = std::min<uint32_t>(f.lines.line_count(), row.line + 2);
            for (uint32_t line = lo; line <= hi; ++line) {
                std::vector<std::string> ts; tokens(f.lines.line_text(line), ts);
                for (const auto &t : ts) {
                    if (normalized_seeds.count(lower(t))) continue;
                    Related &r = related[t]; r.id = t; ++r.same_window_hits;
                    r.seed_files.insert(f.path);
                }
            }
            if (row.enclosing) {
                std::string key = f.path + "\n" + std::to_string(row.enclosing->from);
                ScopeEvidence &s = scopes[key];
                s.file = f.path; s.scope = *row.enclosing; ++s.matches;
                if (seen_scopes.insert(row.enclosing->from).second &&
                    row.enclosing->to <= f.content.size()) {
                    std::vector<std::string> ts;
                    tokens(std::string_view(f.content).substr(
                               row.enclosing->from,
                               row.enclosing->to - row.enclosing->from), ts);
                    for (const auto &t : ts) {
                        if (normalized_seeds.count(lower(t))) continue;
                        Related &r = related[t]; r.id = t; ++r.same_scope_hits;
                        r.seed_files.insert(f.path);
                    }
                }
            }
        }
        for (auto &[id, r] : related) {
            auto it = file_counts.find(id);
            if (it != file_counts.end()) {
                r.same_file_hits += it->second;
                r.seed_files.insert(f.path);
            }
        }
    }

    std::vector<Related *> candidates;
    for (auto &[id, r] : related) candidates.push_back(&r);
    auto prelim = [](const Related *a, const Related *b) {
        const double sa = a->same_scope_hits * 4.0 + a->same_window_hits * 2.0 +
                          a->same_file_hits * 0.5 + a->seed_files.size() * 2.0;
        const double sb = b->same_scope_hits * 4.0 + b->same_window_hits * 2.0 +
                          b->same_file_hits * 0.5 + b->seed_files.size() * 2.0;
        if (sa != sb) return sa > sb;
        return a->id < b->id;
    };
    std::sort(candidates.begin(), candidates.end(), prelim);
    uint64_t candidate_patterns_omitted = 0;
    if (candidates.size() > static_cast<size_t>(cli.investigate.max_related_patterns)) {
        candidate_patterns_omitted = candidates.size() - cli.investigate.max_related_patterns;
        candidates.resize(cli.investigate.max_related_patterns);
        stats.rows_truncated += candidate_patterns_omitted;
        if (stats.stop_reason.empty()) stats.stop_reason = "adaptive_pattern_cap";
    }

    bool follow = cli.investigate.followup == InvestigateOptions::Followup::Always ||
                  (cli.investigate.followup == InvestigateOptions::Followup::Auto &&
                   !candidates.empty());
    uint64_t corpus_file_count = stats.files_scanned;
    if (follow && !candidates.empty()) {
        std::vector<Pattern> rel_patterns;
        for (size_t i = 0; i < candidates.size(); ++i) {
            Pattern p;
            p.id = "related" + std::to_string(i);
            p.regexp = regex_escape(candidates[i]->id);
            p.word_boundary = true;
            rel_patterns.push_back(std::move(p));
        }
        Matcher rel_matcher; CompileError ce;
        if (!rel_matcher.compile(rel_patterns, &ce)) {
            std::fprintf(stderr, "hprscript: related matcher compile failed: %s\n", ce.message.c_str());
            return 2;
        }
        ++stats.matcher_compilations;
        stats.patterns_compiled += rel_patterns.size();
        ++stats.scan_stages;
        corpus_file_count = 0;
        auto rel_scan = [&](const std::string &path, std::string_view content) {
            std::vector<char> hit(candidates.size(), 0);
            uint64_t found = 0;
            rel_matcher.scan(content, [&](const Match &m) {
                if (m.pattern_index < hit.size()) hit[m.pattern_index] = 1;
                ++found; return true;
            });
            ++corpus_file_count;
            stats.rows_materialized += found;
            for (size_t i = 0; i < hit.size(); ++i)
                if (hit[i]) candidates[i]->corpus_files.insert(path);
        };
        if (from_stdin) {
            ++stats.files_scanned;
            stats.bytes_scanned += stdin_content.size();
            rel_scan("<stdin>", stdin_content);
        } else {
            Walker again;
            ScanStats discovery;
            std::unordered_map<std::string, AddedLines> unused;
            if (!add_walker_inputs(cli, again, discovery, unused)) return 2;
            again.walk([&](const WalkItem &it) {
                MappedFile mf;
                if (!mf.open(it.path) || looks_binary(mf.view())) return true;
                ++stats.files_scanned;
                stats.bytes_scanned += mf.view().size();
                rel_scan(it.path, mf.view()); return true;
            });
        }
    } else {
        for (Related *r : candidates) r->corpus_files = r->seed_files;
    }

    for (Related *r : candidates) {
        const double assoc = r->same_scope_hits * 4.0 + r->same_window_hits * 2.0 +
                             r->same_file_hits * 0.5 + r->seed_files.size() * 2.0;
        const double rarity = std::log((static_cast<double>(corpus_file_count) + 1.0) /
                                       (static_cast<double>(r->corpus_files.size()) + 1.0)) + 1.0;
        r->score = assoc * rarity;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Related *a, const Related *b) {
        if (a->score != b->score) return a->score > b->score;
        if (a->seed_files.size() != b->seed_files.size()) return a->seed_files.size() > b->seed_files.size();
        if (a->same_scope_hits != b->same_scope_hits) return a->same_scope_hits > b->same_scope_hits;
        if (a->corpus_files.size() != b->corpus_files.size()) return a->corpus_files.size() < b->corpus_files.size();
        return a->id < b->id;
    });
    uint64_t related_limit_omitted = 0;
    if (candidates.size() > static_cast<size_t>(cli.investigate.related)) {
        related_limit_omitted = candidates.size() - cli.investigate.related;
        candidates.resize(cli.investigate.related);
    }

    std::vector<RankRow> ranked_files = rank_files(ranking);
    for (auto &row : ranked_files) {
        FileRoleResult roles = classify_file_roles(row.file);
        if (profile == "config") {
            if (has_role(roles, "config")) row.score += 1.0;
            if (has_role(roles, "documentation")) row.score += 0.25;
        } else if (profile == "symbol") {
            if (has_role(roles, "source")) row.score += 0.25;
            if (has_role(roles, "test")) row.score += 0.35;
        } else if (profile == "error") {
            if (has_role(roles, "test")) row.score += 0.4;
            if (has_role(roles, "source")) row.score += 0.2;
        } else if (profile == "concept") {
            if (has_role(roles, "documentation")) row.score += 0.3;
            if (has_role(roles, "test")) row.score += 0.2;
        }
    }
    std::sort(ranked_files.begin(), ranked_files.end(), [](const RankRow &a,
                                                            const RankRow &b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.density != b.density) return a.density > b.density;
        return a.file < b.file;
    });
    uint64_t top_files_limit_omitted = 0;
    if (ranked_files.size() > static_cast<size_t>(cli.investigate.top_files)) {
        top_files_limit_omitted = ranked_files.size() - cli.investigate.top_files;
        ranked_files.resize(cli.investigate.top_files);
    }
    std::map<std::string, double> file_scores;
    for (const auto &r : ranked_files) file_scores[r.file] = r.score;
    std::vector<ScopeEvidence *> ranked_scopes;
    for (auto &[key, s] : scopes) {
        s.score = static_cast<double>(s.matches) + file_scores[s.file];
        ranked_scopes.push_back(&s);
    }
    std::sort(ranked_scopes.begin(), ranked_scopes.end(), [](const auto *a, const auto *b) {
        if (a->score != b->score) return a->score > b->score;
        if (a->file != b->file) return a->file < b->file;
        return a->scope.from < b->scope.from;
    });
    uint64_t top_scopes_limit_omitted = 0;
    if (ranked_scopes.size() > static_cast<size_t>(cli.investigate.top_scopes)) {
        top_scopes_limit_omitted = ranked_scopes.size() - cli.investigate.top_scopes;
        ranked_scopes.resize(cli.investigate.top_scopes);
    }

    std::vector<const MatchRow *> evidence;
    std::map<uint64_t, OccurrenceClassification> classes;
    uint64_t definitions = 0, references = 0, test_files = 0, config_files = 0;
    std::set<std::string> tests_seen, configs_seen;
    for (const auto &f : files) {
        if (has_role(f.roles, "test")) tests_seen.insert(f.path);
        if (has_role(f.roles, "config")) configs_seen.insert(f.path);
        for (const auto &row : f.rows) {
            auto c = classify_occurrence(f.path, row.context, row.match, profile,
                                         has_role(f.roles, "test"));
            if (c.classification == "probable_definition" ||
                c.classification == "probable_declaration") ++definitions;
            else ++references;
            classes[row.row_id] = std::move(c);
            evidence.push_back(&row);
        }
    }
    test_files = tests_seen.size(); config_files = configs_seen.size();
    std::stable_sort(evidence.begin(), evidence.end(), [&](const MatchRow *a, const MatchRow *b) {
        const auto is_definition = [&](const MatchRow *row) {
            const std::string &kind = classes[row->row_id].classification;
            return kind == "probable_definition" || kind == "probable_declaration";
        };
        bool ad = is_definition(a);
        bool bd = is_definition(b);
        if (ad != bd) return ad;
        auto emphasis = [&](const MatchRow *row) {
            const std::string text = lower(row->context);
            if (profile == "error")
                return text.find("error") != std::string::npos ||
                       text.find("throw") != std::string::npos ||
                       text.find("wrap") != std::string::npos ||
                       text.find("log") != std::string::npos ||
                       text.find("assert") != std::string::npos;
            if (profile == "config")
                return text.find("getenv") != std::string::npos ||
                       text.find("config") != std::string::npos ||
                       text.find('=') != std::string::npos;
            return false;
        };
        bool ae = emphasis(a), be = emphasis(b);
        if (ae != be) return ae;
        double as = file_scores[a->file], bs = file_scores[b->file];
        if (as != bs) return as > bs;
        if (a->file != b->file) return a->file < b->file;
        return a->from < b->from;
    });
    uint64_t examples_limit_omitted = 0;
    if (evidence.size() > static_cast<size_t>(cli.investigate.examples)) {
        examples_limit_omitted = evidence.size() - cli.investigate.examples;
        evidence.resize(cli.investigate.examples);
    }

    if ((stats.files_failed || stats.missing_paths) && stats.stop_reason.empty())
        stats.stop_reason = "input_failure";

    const bool llm = cli.out_mode == OutputMode::Llm;
    const uint64_t files_with_seed = ranking.file_order.size();
    std::string summary;
    if (llm) {
        summary = "INVESTIGATION profile=" + profile + " seeds=";
        for (size_t i = 0; i < seeds.size(); ++i) { if (i) summary += ','; summary += seeds[i]; }
        summary += " scans=" + std::to_string(stats.scan_stages) +
                   " complete=" + (stats.complete() ? "yes\n\n" : "no\n\n");
        summary += "SUMMARY\n" + std::to_string(stats.matches_seen) +
                   " seed matches in " + std::to_string(files_with_seed) +
                   " files; " + std::to_string(definitions) +
                   " probable definitions/declarations; " + std::to_string(test_files) +
                   " test files; " + std::to_string(config_files) + " config files.\n\n";
    } else {
        summary = "{\"type\":\"investigation-summary\",\"profile\":\"";
        json_escape_to(summary, profile); summary += "\",\"seeds\":" + json_array(seeds);
        summary += ",\"files_with_seed\":" + std::to_string(files_with_seed);
        summary += ",\"seed_matches\":" + std::to_string(stats.matches_seen);
        summary += ",\"probable_definitions\":" + std::to_string(definitions);
        summary += ",\"probable_references\":" + std::to_string(references);
        summary += ",\"tests\":" + std::to_string(test_files);
        summary += ",\"configs\":" + std::to_string(config_files);
        summary += ",\"scan_stages\":" + std::to_string(stats.scan_stages);
        summary += ",\"complete\":" + std::string(stats.complete() ? "true" : "false") + "}\n";
    }

    struct ReportPiece {
        std::string text;
        bool selected{false};
        bool definition{false};
    };
    std::vector<ReportPiece> file_pieces;
    std::vector<ReportPiece> scope_pieces;
    std::vector<ReportPiece> related_pieces;
    std::vector<ReportPiece> evidence_pieces;

    for (size_t i = 0; i < ranked_files.size(); ++i) {
        const auto &r = ranked_files[i];
        FileRoleResult roles = classify_file_roles(r.file);
        std::string s;
        if (llm) {
            s = std::to_string(i + 1) + ". " + r.file + " score=" + std::to_string(r.score) + " roles=";
            for (size_t j = 0; j < roles.roles.size(); ++j) { if (j) s += ','; s += roles.roles[j]; }
            s += " best=" + std::to_string(r.window_lo) + "-" + std::to_string(r.window_hi) + "\n";
        } else {
            s = "{\"type\":\"investigation-file\",\"file\":\"";
            json_escape_to(s, r.file); s += "\",\"score\":" + std::to_string(r.score);
            s += ",\"roles\":" + json_array(roles.roles) +
                 ",\"role_method\":\"path-heuristic\",\"best_line_start\":" +
                 std::to_string(r.window_lo) + ",\"best_line_end\":" +
                 std::to_string(r.window_hi) + "}\n";
        }
        file_pieces.push_back({std::move(s), false, false});
    }
    for (const auto *r : ranked_scopes) {
        std::string s;
        if (llm) {
            s = r->file + ":" + std::to_string(r->scope.line_start) + " " +
                r->scope.kind + " " + r->scope.name + " matches=" +
                std::to_string(r->matches) + "\n";
        } else {
            s = "{\"type\":\"investigation-scope\",\"file\":\"";
            json_escape_to(s, r->file); s += "\",\"name\":\"";
            json_escape_to(s, r->scope.name); s += "\",\"kind\":\"";
            json_escape_to(s, r->scope.kind); s += "\",\"line_start\":" +
                std::to_string(r->scope.line_start) + ",\"line_end\":" +
                std::to_string(r->scope.line_end) + ",\"matches\":" +
                std::to_string(r->matches) + "}\n";
        }
        scope_pieces.push_back({std::move(s), false, false});
    }
    for (const Related *r : candidates) {
        std::string s;
        if (llm) {
            s = r->id + " score=" + std::to_string(r->score) + " same_scope=" +
                std::to_string(r->same_scope_hits) + " files=" +
                std::to_string(r->corpus_files.size()) + "\n";
        } else {
            s = "{\"type\":\"investigation-related\",\"identifier\":\"";
            json_escape_to(s, r->id); s += "\",\"score\":" + std::to_string(r->score);
            s += ",\"same_scope_hits\":" + std::to_string(r->same_scope_hits);
            s += ",\"same_window_hits\":" + std::to_string(r->same_window_hits);
            s += ",\"same_file_hits\":" + std::to_string(r->same_file_hits);
            s += ",\"seed_files\":" + std::to_string(r->seed_files.size());
            s += ",\"corpus_files\":" + std::to_string(r->corpus_files.size()) + "}\n";
        }
        related_pieces.push_back({std::move(s), false, false});
    }
    for (const MatchRow *r : evidence) {
        const auto &c = classes[r->row_id];
        std::string s;
        if (llm) {
            s = r->file + ":" + std::to_string(r->line) + " [" +
                c.classification + ", confidence=" + c.confidence + "] " +
                r->context + "\n";
        } else {
            s = "{\"type\":\"investigation-evidence\",\"file\":\"";
            json_escape_to(s, r->file); s += "\",\"line\":" + std::to_string(r->line);
            s += ",\"column\":" + std::to_string(r->column) + ",\"pattern_id\":\"";
            json_escape_to(s, r->pattern_id); s += "\",\"classification\":\"";
            json_escape_to(s, c.classification); s += "\",\"confidence\":\"";
            json_escape_to(s, c.confidence); s += "\",\"method\":\"";
            json_escape_to(s, c.method); s += "\",\"context\":\"";
            json_escape_to(s, r->context); s += "\"}\n";
        }
        const bool definition = c.classification == "probable_definition" ||
                                c.classification == "probable_declaration";
        evidence_pieces.push_back({std::move(s), false, definition});
    }

    // Reserve the one extra byte needed if JSON complete:true becomes false
    // after budget selection discovers omissions.
    uint64_t used = summary.size() + (llm ? 0 : 1);
    const auto fits = [&](uint64_t bytes) {
        return !evidence_budget || used + bytes <= evidence_budget;
    };
    auto select_section = [&](std::vector<ReportPiece> &pieces,
                              const std::string &header,
                              const auto &include) {
        bool header_selected = false;
        for (auto &piece : pieces) {
            if (!include(piece)) continue;
            uint64_t required = piece.text.size();
            if (llm && !header_selected) required += header.size();
            if (!fits(required)) continue;
            if (llm && !header_selected) {
                used += header.size();
                header_selected = true;
            }
            used += piece.text.size();
            piece.selected = true;
        }
        return header_selected;
    };

    // Selection priority is deliberately independent of render order. JSONL
    // retains its stable type ordering while scarce bytes are reserved for
    // the evidence an agent is least able to recover from rankings alone.
    const bool definitions_header = select_section(
        evidence_pieces, "\nPROBABLE DEFINITIONS / DECLARATIONS\n",
        [](const ReportPiece &piece) { return piece.definition; });
    const bool files_header = select_section(
        file_pieces, "TOP FILES\n",
        [](const ReportPiece &) { return true; });
    const bool scopes_header = select_section(
        scope_pieces, "\nTOP SCOPES\n",
        [](const ReportPiece &) { return true; });
    const bool related_header = select_section(
        related_pieces, "\nRELATED IDENTIFIERS\n",
        [](const ReportPiece &) { return true; });
    const bool evidence_header = select_section(
        evidence_pieces, "\nREPRESENTATIVE EVIDENCE\n",
        [](const ReportPiece &piece) { return !piece.definition; });

    uint64_t omitted_files = 0, omitted_scopes = 0, omitted_related = 0, omitted_evidence = 0;
    auto count_omitted = [](const std::vector<ReportPiece> &pieces) {
        uint64_t count = 0;
        for (const auto &piece : pieces) if (!piece.selected) ++count;
        return count;
    };
    omitted_files = count_omitted(file_pieces);
    omitted_scopes = count_omitted(scope_pieces);
    omitted_related = count_omitted(related_pieces);
    omitted_evidence = count_omitted(evidence_pieces);

    const uint64_t omitted = omitted_files + omitted_scopes + omitted_related + omitted_evidence;
    if (omitted) {
        stats.rows_truncated += omitted;
        if (stats.stop_reason.empty()) stats.stop_reason = "evidence_budget";
    }
    if (!stats.complete()) {
        const std::string before = llm ? "complete=yes" : "\"complete\":true";
        const std::string after = llm ? "complete=no" : "\"complete\":false";
        if (size_t pos = summary.find(before); pos != std::string::npos)
            summary.replace(pos, before.size(), after);
    }

    auto emit_selected = [&](const std::vector<ReportPiece> &pieces) {
        for (const auto &piece : pieces) {
            if (!piece.selected) continue;
            std::fwrite(piece.text.data(), 1, piece.text.size(), stdout);
            ++stats.rows_output;
        }
    };
    std::fwrite(summary.data(), 1, summary.size(), stdout); ++stats.rows_output;
    if (llm && files_header) std::fputs("TOP FILES\n", stdout);
    emit_selected(file_pieces);
    if (llm && scopes_header) std::fputs("\nTOP SCOPES\n", stdout);
    emit_selected(scope_pieces);
    if (llm && definitions_header)
        std::fputs("\nPROBABLE DEFINITIONS / DECLARATIONS\n", stdout);
    for (const auto &piece : evidence_pieces) {
        if (piece.selected && piece.definition) {
            std::fwrite(piece.text.data(), 1, piece.text.size(), stdout);
            ++stats.rows_output;
        }
    }
    if (llm && related_header) std::fputs("\nRELATED IDENTIFIERS\n", stdout);
    emit_selected(related_pieces);
    if (llm && evidence_header) std::fputs("\nREPRESENTATIVE EVIDENCE\n", stdout);
    for (const auto &piece : evidence_pieces) {
        if (piece.selected && !piece.definition) {
            std::fwrite(piece.text.data(), 1, piece.text.size(), stdout);
            ++stats.rows_output;
        }
    }
    std::string footer;
    if (llm) {
        footer = "\nLIMITS\n";
        footer += omitted ? std::to_string(omitted) + " records omitted by evidence budget.\n"
                          : "No sections truncated.\n";
        footer += "candidate_patterns_omitted=" + std::to_string(candidate_patterns_omitted) +
                  " top_files_omitted=" + std::to_string(top_files_limit_omitted) +
                  " top_scopes_omitted=" + std::to_string(top_scopes_limit_omitted) +
                  " related_omitted=" + std::to_string(related_limit_omitted) +
                  " examples_omitted=" + std::to_string(examples_limit_omitted) + "\n";
        if (!stats.stop_reason.empty()) footer += "stop_reason=" + stats.stop_reason + "\n";
    } else {
        footer = "{\"type\":\"investigation-footer\",\"complete\":";
        footer += stats.complete() ? "true" : "false";
        footer += ",\"omitted_files\":" + std::to_string(omitted_files) +
                  ",\"omitted_scopes\":" + std::to_string(omitted_scopes) +
                  ",\"omitted_related\":" + std::to_string(omitted_related) +
                  ",\"omitted_evidence\":" + std::to_string(omitted_evidence) +
                  ",\"candidate_patterns_omitted\":" + std::to_string(candidate_patterns_omitted) +
                  ",\"top_files_limit_omitted\":" + std::to_string(top_files_limit_omitted) +
                  ",\"top_scopes_limit_omitted\":" + std::to_string(top_scopes_limit_omitted) +
                  ",\"related_limit_omitted\":" + std::to_string(related_limit_omitted) +
                  ",\"examples_limit_omitted\":" + std::to_string(examples_limit_omitted);
        if (!stats.stop_reason.empty()) {
            footer += ",\"stop_reason\":\"";
            json_escape_to(footer, stats.stop_reason); footer += '"';
        }
        footer += "}\n";
    }
    std::fwrite(footer.data(), 1, footer.size(), stdout); ++stats.rows_output;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (cli.summary) emit_summary_record(stats, stats.rows_output, elapsed);
    if (cli.require_complete && !stats.complete()) return 2;
    return stats.matches_seen ? 0 : 1;
}

} // namespace hpr
