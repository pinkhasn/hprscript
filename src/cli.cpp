#include "cli.hpp"

#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace hpr {

namespace {

bool eq(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// Pull the next argv slot as a value; sets cli.error and returns nullptr if
// argv is exhausted.
const char *take(int &i, int argc, char **argv, const char *flag, Cli &cli) {
    if (i + 1 >= argc) {
        cli.error = true;
        cli.error_message = std::string("flag ") + flag + " requires a value";
        return nullptr;
    }
    return argv[++i];
}

// Regex-escape a fixed string so it can join the normal compile path (-F).
// Only ASCII metacharacters are escaped — UTF-8 bytes pass through intact.
std::string escape_literal(const char *s) {
    static const char specials[] = "\\^$.[]|()?*+{}";
    std::string out;
    out.reserve(std::strlen(s) + 8);
    for (; *s; ++s) {
        if (std::strchr(specials, *s)) out += '\\';
        out += *s;
    }
    return out;
}

// A pattern name must look like an identifier so `$PAT_ID`, `[name]` tags
// and the A:B relation syntax stay unambiguous.
bool valid_pattern_name(const char *s) {
    if (!*s || (!std::isalpha((unsigned char)*s) && *s != '_')) return false;
    for (; *s; ++s)
        if (!std::isalnum((unsigned char)*s) && *s != '_') return false;
    return true;
}

// Strict non-negative integer parse for guard flags (-expect,
// -max-span-lines): a typo must be an error, not a silent 0.
bool parse_nonneg(const char *s, int64_t &out) {
    if (!*s) return false;
    int64_t v = 0;
    for (; *s; ++s) {
        if (!std::isdigit((unsigned char)*s)) return false;
        v = v * 10 + (*s - '0');
        if (v < 0) return false; // overflow
    }
    out = v;
    return true;
}

bool set_output_mode(Cli &cli, OutputMode mode) {
    if (cli.out_mode_set) {
        cli.error = true;
        cli.error_message = "output modes -j/-f/-c/-o/-format/-absent/-llm are mutually exclusive";
        return false;
    }
    cli.out_mode = mode;
    cli.out_mode_set = true;
    return true;
}

// Validate edit-subcommand flag coherence (HPRSCRIPT.md, "Edit mode" chapter). Called at
// the end of parse_cli when the edit subcommand is active. Every check here
// is a usage error (exit 2); guard violations at run time exit 3 instead.
void validate_edit_cli(Cli &cli) {
    auto fail = [&](const std::string &msg) {
        cli.error = true;
        cli.error_message = msg;
    };
    const EditOptions &e = cli.edit;

    if (cli.out_mode_set && cli.out_mode != OutputMode::JsonLines)
        return fail("edit mode: output is a dry-run diff or -j edit records; "
                    "-f/-c/-o/-llm/-absent/-format do not apply");
    if (cli.sample_n > 0)
        return fail("edit mode cannot combine with -sample");
    if (cli.records != Cli::RecordMode::None)
        return fail("edit mode cannot combine with -records");
    if (cli.context_before != 0 || cli.context_after != 0)
        return fail("edit mode: -A/-B/-C do not apply "
                    "(the diff carries its own context)");
    if (!cli.script_inline.empty() || !cli.script_path.empty())
        return fail("edit mode cannot combine with -s/-script "
                    "(the script DSL is read-only)");

    if ((e.span == EditOptions::Span::Block ||
         e.span == EditOptions::Span::BlockFull) &&
        (cli.block_open.empty() || cli.block_close.empty()))
        return fail("-span block/block-full requires -block-open and "
                    "-block-close");
    const bool have_scope_cfg =
        !cli.scope_lang.empty() ||
        (!cli.scope_pattern.empty() && !cli.scope_open.empty() &&
         !cli.scope_close.empty()) ||
        !cli.in_scopes.empty() || !cli.in_scope_kind.empty();
    if ((e.span == EditOptions::Span::Scope ||
         e.span == EditOptions::Span::ScopeBody) && !have_scope_cfg)
        return fail("-span scope/scope-body requires an active -scope "
                    "(built-in pack, -scope-pattern/-scope-open/-scope-close, "
                    "or -in-scope which implies -scope auto)");

    const int sources = (e.content_set ? 1 : 0) +
                        (e.content_file.empty() ? 0 : 1) +
                        (e.content_stdin ? 1 : 0);
    if (e.verb == EditOptions::Verb::Delete) {
        if (sources != 0)
            return fail("-delete does not take "
                        "-content/-content-file/-content-stdin");
    } else if (sources == 0) {
        return fail("edit mode needs a content source: one of -content, "
                    "-content-file, -content-stdin (or -delete)");
    } else if (sources > 1) {
        return fail("give exactly one of -content, -content-file, "
                    "-content-stdin");
    }

    if (e.verb == EditOptions::Verb::Insert &&
        (e.insert_pos == EditOptions::InsertPos::Start ||
         e.insert_pos == EditOptions::InsertPos::End) &&
        e.span != EditOptions::Span::Block &&
        e.span != EditOptions::Span::ScopeBody)
        return fail("-insert start/end needs a delimited span "
                    "(-span block or -span scope-body); "
                    "use before/after otherwise");

    bool any_target = cli.patterns.empty(); // no patterns is caught later
    for (const auto &p : cli.patterns)
        if (!p.ref) any_target = true;
    if (!any_target)
        return fail("every pattern is -ref (reference-only); "
                    "at least one pattern must produce edits");

    const bool have_inputs = !cli.positional.empty() || !cli.globs.empty() ||
                             !cli.file_lists.empty() || cli.git_changed ||
                             cli.git_staged || cli.git_untracked ||
                             !cli.git_ranges.empty();
    if (!have_inputs)
        return fail("edit mode requires explicit input files (positional "
                    "paths, -glob, -files-from, or -git-*); it never reads "
                    "a scan target from stdin");
}

} // namespace

void print_help(FILE *out) {
    std::fprintf(out,
"hprscript — multi-pattern PCRE search powered by Vectorscan\n"
"\n"
"Usage:\n"
"  hprscript -p <pattern> [options] [files/dirs...]\n"
"  hprscript -s '<json>' [files...]\n"
"  hprscript -script <path> [files...]\n"
"  hprscript [script.json]            # positional script or stdin\n"
"  hprscript edit -p <pat> <edit flags> [files...]   # the ONLY write mode\n"
"\n"
"Search flags (with -p):\n"
"  -p <pattern>     Search pattern (PCRE; repeatable for multi-pattern)\n"
"  -pi <pattern>    Case-insensitive search pattern (repeatable)\n"
"  -F <string>      Fixed-string pattern — no regex interpretation (repeatable)\n"
"  -Fi <string>     Case-insensitive fixed-string pattern\n"
"  -name <id>       Name the preceding pattern (shown as pat/$PAT_ID, usable\n"
"                   as the A/B side of relations and in -file-where)\n"
"  -patterns-from <f>  Load patterns from a JSONL rule file: one object per\n"
"                   line {id, regexp|literal, case_insensitive, word_boundary,\n"
"                   utf8}; '#' comment lines allowed (repeatable)\n"
"  -extract n1,n2,…  Re-extract capture groups from the preceding -p/-pi\n"
"  -glob <glob>     Scan glob (e.g. \"**/*.go\"; repeatable)\n"
"  -exclude <pat>   Exclude rule: glob, bare dir name, or path prefix (repeatable)\n"
"  -files-from <f>  Scan the literal paths listed in f, one per line ('-' = stdin)\n"
"  -files0-from <f> Same, NUL-separated — safe for any filename (find -print0,\n"
"                   git diff --name-only -z). No glob interpretation. Repeatable.\n"
"  -git-changed     Scan files changed vs HEAD (staged + unstaged)\n"
"  -git-staged      Scan files staged for commit (diff --cached)\n"
"  -git-untracked   Scan untracked files (ls-files -o --exclude-standard)\n"
"  -git-range <r>   Scan files changed in a diff range (e.g. main...HEAD)\n"
"  -git-added-lines Restrict matches to lines ADDED by the selected diffs\n"
"                   (needs -git-changed/-staged/-range; -p mode only)\n"
"  -w               Whole-word match (\\b…\\b)\n"
"  -no-utf8         Disable UTF-8 mode (byte-level matching)\n"
"  -ucp             Enable Unicode \\w/\\d/\\s (may reject some patterns)\n"
"  -limit <n>       Max global results\n"
"  -m <n>           Max results per file\n"
"  -context <n>     Symmetric context lines (also -C)\n"
"  -A <n>           Lines after match\n"
"  -B <n>           Lines before match\n"
"\n"
"Output modes (mutually exclusive; default is JSON Lines):\n"
"  -j               JSON Lines (default; explicit form)\n"
"  -f               File paths only (deduped, like grep -l)\n"
"  -c               Count matches per file (like grep -c)\n"
"  -o               Matched text only (like grep -o)\n"
"  -format <tmpl>   Custom: $FILE $LINE $COL $MATCH $CONTEXT $FROM $TO $PAT_ID\n"
"                   ($BLOCK / $BLOCK_FULL / $BLOCK_START / $BLOCK_END\n"
"                   / $BLOCK_LINE_START / $BLOCK_LINE_END when block-extracting)\n"
"  -absent          Files where pattern is NOT found (like grep -L)\n"
"  -records line    With -absent: one JSON record per non-empty line lacking\n"
"                   each pattern (record-level absence, e.g. JSONL fields)\n"
"  -llm             Token-efficient text for LLM consumption (auto-detects\n"
"                   block/scope; dedupes file paths; prints a 'limit reached'\n"
"                   footer when -limit or -max-output-bytes truncates output)\n"
"\n"
"Block extraction (with -p):\n"
"  -block-open <s>   Opening delimiter (e.g. \"{\")\n"
"  -block-close <s>  Closing delimiter (e.g. \"}\"). Both required.\n"
"\n"
"Byte budgets (0 = unlimited):\n"
"  -max-match-bytes <n>     Truncate $MATCH at n bytes (UTF-8 safe)\n"
"  -max-context-bytes <n>   Truncate $CONTEXT at n bytes\n"
"  -max-block-bytes <n>     Truncate $BLOCK / $BLOCK_FULL at n bytes\n"
"  -max-output-bytes <n>    Stop scanning once total stdout exceeds n bytes\n"
"\n"
"Sample mode:\n"
"  -sample <n>      Buffer & return n diverse matches (file/shape-stratified)\n"
"\n"
"Pattern relations (filter matches by proximity/containment, repeatable):\n"
"  -near A:B:K      Emit pattern A's matches with a B-match within K lines\n"
"  -far  A:B:K      Emit A's matches with NO B-match within K lines (K=0=same)\n"
"  -same-scope A:B      Emit A's matches with a B-match inside the same\n"
"                       enclosing scope (requires -scope)\n"
"  -not-same-scope A:B  Emit A's matches with NO B-match in the same scope\n"
"\n"
"Per-file filter:\n"
"  -file-where <expr>   Emit a file's matches only when the predicate over\n"
"                       matched patterns holds: 'err AND NOT recovery',\n"
"                       AND/OR/NOT or &&/||/!, parentheses, ids or p0/p1…\n"
"\n"
"Enclosing-scope annotation (with -p):\n"
"  -scope <lang|auto>       Built-in pack (auto, go, rust, c, cpp, java, js, ts)\n"
"  -scope-pattern <regex>   Custom scope-anchor regex (capture group 1 = name)\n"
"  -scope-open <s>          Body opening delimiter for the custom anchor\n"
"  -scope-close <s>         Body closing delimiter\n"
"  -scope-kind <s>          Label emitted as enclosing.kind (default 'func')\n"
"\n"
"Scoped targeting (search and edit modes; imply -scope auto):\n"
"  -in-scope <re>       Keep only matches inside a scope whose name matches\n"
"                       (checks the whole enclosing chain; repeatable = OR)\n"
"  -in-scope-kind <k>   Restrict -in-scope to scopes of this kind\n"
"  -lines <spec>        Keep only matches starting in these 1-based lines:\n"
"                       N, A:B, A:, or :B (repeatable = OR)\n"
"  -list-scopes         List the scope index instead of searching: one\n"
"                       record per function/class (JSONL, or -llm flat).\n"
"                       Honors -in-scope/-in-scope-kind; takes no patterns\n"
"\n"
"Script mode:\n"
"  -s <json>        Inline script\n"
"  -script <path>   Script file\n"
"\n"
"Scan accounting (work in -p and script mode):\n"
"  -summary            Emit a trailing {\"type\":\"summary\",...} record with\n"
"                      files scanned/skipped/failed, matches, completeness\n"
"  -diagnostics        Emit {\"type\":\"warning\",...} records on stdout for\n"
"                      read errors, binary skips, missing list paths\n"
"  -require-complete   Exit 2 when any file couldn't be read or a listed\n"
"                      path was missing (partial results become failures)\n"
"\n"
"Edit mode (hprscript edit …; search/script modes never modify files):\n"
"  Dry-run by default: prints a unified diff of would-be changes plus a\n"
"  summary record; nothing is written without -write. Targeting flags\n"
"  (-p/-glob/-near/-far/-file-where/-git-*/…) work as in search mode.\n"
"  -span <s>            What each match edits: match (default), line, block,\n"
"                       block-full ($BLOCK/$BLOCK_FULL ranges), scope (whole\n"
"                       enclosing function), scope-body (its body block).\n"
"                       With -in-scope and no -p at all, scope spans edit the\n"
"                       named scopes directly (anchorless)\n"
"  -content <tmpl>      Replacement template ($MATCH, $FILE, $EXTRACT_*, $$)\n"
"  -content-file <f>    Replacement content from a file, verbatim — the way\n"
"                       to pass multi-line code (no shell quoting)\n"
"  -content-stdin       Replacement content from stdin\n"
"  -insert <pos>        Insert instead of replace: before|after (span edges),\n"
"                       start|end (just inside block/scope-body delimiters)\n"
"  -delete              Remove the span (-span line also removes the newline)\n"
"  -write               Apply changes (atomic per file: temp + rename)\n"
"  -diff                With -write: also print the unified diff\n"
"  -ref                 Mark the preceding pattern reference-only: it can\n"
"                       qualify edits via -near/-far/-file-where but its own\n"
"                       matches are never edited\n"
"  -expect <n>          Guard: refuse (exit 3) unless exactly n edit sites\n"
"  -max-span-lines <n>  Guard: refuse spans over n lines (default 500, 0=off)\n"
"  -assert-contains <re> Guard: refuse unless every target span matches\n"
"  -j                   Emit JSONL edit records instead of the dry-run diff\n"
"\n"
"Misc:\n"
"  --version        Show version\n"
"  -h, --help       This help\n");
}

Cli parse_cli(int argc, char **argv) {
    Cli cli;
    int first = 1;
    // Subcommand detection: `hprscript edit …`. Must be argv[1] so
    // command-prefix permission rules can distinguish write-capable
    // invocations. A file literally named "edit" can still be scanned via
    // `hprscript -p pat -- edit`.
    if (argc > 1 && eq(argv[1], "edit")) {
        cli.edit.active = true;
        first = 2;
    }
    for (int i = first; i < argc; ++i) {
        const char *a = argv[i];
        if (eq(a, "--version")) { cli.show_version = true; continue; }
        if (eq(a, "-h") || eq(a, "--help")) { cli.show_help = true; continue; }

        if (eq(a, "-p")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            CliPattern p; p.regexp = v; p.case_insensitive = false;
            cli.patterns.push_back(std::move(p));
            continue;
        }
        if (eq(a, "-pi")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            CliPattern p; p.regexp = v; p.case_insensitive = true;
            cli.patterns.push_back(std::move(p));
            continue;
        }
        if (eq(a, "-F")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            CliPattern p; p.regexp = escape_literal(v);
            cli.patterns.push_back(std::move(p));
            continue;
        }
        if (eq(a, "-Fi")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            CliPattern p; p.regexp = escape_literal(v); p.case_insensitive = true;
            cli.patterns.push_back(std::move(p));
            continue;
        }
        if (eq(a, "-patterns-from") || eq(a, "--patterns-from")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.patterns_from.emplace_back(v);
            continue;
        }
        if (eq(a, "-extract")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            if (cli.patterns.empty()) {
                cli.error = true;
                cli.error_message = "-extract must follow a -p/-pi";
                return cli;
            }
            CliPattern &last = cli.patterns.back();
            if (!last.extract_names.empty()) {
                cli.error = true;
                cli.error_message = "-extract repeated for the same pattern";
                return cli;
            }
            // Split comma-separated list. Empty names are rejected.
            const char *s = v;
            std::string cur;
            for (; *s; ++s) {
                if (*s == ',') {
                    if (cur.empty()) {
                        cli.error = true;
                        cli.error_message = "-extract: empty name";
                        return cli;
                    }
                    last.extract_names.push_back(std::move(cur));
                    cur.clear();
                } else {
                    cur += *s;
                }
            }
            if (cur.empty()) {
                cli.error = true;
                cli.error_message = "-extract: empty name";
                return cli;
            }
            last.extract_names.push_back(std::move(cur));
            continue;
        }
        if (eq(a, "-name")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            if (cli.patterns.empty()) {
                cli.error = true;
                cli.error_message = "-name must follow a -p/-pi/-F/-Fi";
                return cli;
            }
            if (!cli.patterns.back().name.empty()) {
                cli.error = true;
                cli.error_message = "-name repeated for the same pattern";
                return cli;
            }
            if (!valid_pattern_name(v)) {
                cli.error = true;
                cli.error_message = std::string("-name: invalid pattern name '")
                                    + v + "' (use [A-Za-z_][A-Za-z0-9_]*)";
                return cli;
            }
            cli.patterns.back().name = v;
            continue;
        }
        if (eq(a, "-glob")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.globs.emplace_back(v);
            continue;
        }
        if (eq(a, "-exclude")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.excludes.emplace_back(v);
            continue;
        }
        if (eq(a, "-files-from") || eq(a, "--files-from")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.file_lists.push_back({v, false});
            continue;
        }
        if (eq(a, "-files0-from") || eq(a, "--files0-from")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.file_lists.push_back({v, true});
            continue;
        }
        if (eq(a, "-git-changed") || eq(a, "--git-changed")) {
            cli.git_changed = true;
            continue;
        }
        if (eq(a, "-git-staged") || eq(a, "--git-staged")) {
            cli.git_staged = true;
            continue;
        }
        if (eq(a, "-git-untracked") || eq(a, "--git-untracked")) {
            cli.git_untracked = true;
            continue;
        }
        if (eq(a, "-git-range") || eq(a, "--git-range")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.git_ranges.emplace_back(v);
            continue;
        }
        if (eq(a, "-git-added-lines") || eq(a, "--git-added-lines")) {
            cli.git_added_lines = true;
            continue;
        }
        if (eq(a, "-file-where") || eq(a, "--file-where")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            if (!cli.file_where.empty()) {
                cli.error = true;
                cli.error_message = "-file-where given twice";
                return cli;
            }
            cli.file_where = v;
            continue;
        }
        if (eq(a, "-w")) { cli.word_boundary = true; continue; }
        if (eq(a, "-no-utf8")) { cli.no_utf8 = true; continue; }
        if (eq(a, "-ucp"))     { cli.ucp = true; continue; }
        if (eq(a, "-limit")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.limit = std::atoll(v);
            continue;
        }
        if (eq(a, "-m")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.per_file_limit = std::atoll(v);
            continue;
        }
        if (eq(a, "-context") || eq(a, "-C")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            int n = std::atoi(v);
            cli.context_before = cli.context_after = n;
            continue;
        }
        if (eq(a, "-A")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.context_after = std::atoi(v);
            continue;
        }
        if (eq(a, "-B")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.context_before = std::atoi(v);
            continue;
        }

        if (eq(a, "-j")) { if (!set_output_mode(cli, OutputMode::JsonLines)) return cli; continue; }
        if (eq(a, "-f")) { if (!set_output_mode(cli, OutputMode::FilesOnly)) return cli; continue; }
        if (eq(a, "-c")) { if (!set_output_mode(cli, OutputMode::Counts)) return cli; continue; }
        if (eq(a, "-o")) { if (!set_output_mode(cli, OutputMode::MatchOnly)) return cli; continue; }
        if (eq(a, "-absent")) { if (!set_output_mode(cli, OutputMode::Absent)) return cli; continue; }
        if (eq(a, "-llm"))    { if (!set_output_mode(cli, OutputMode::Llm))    return cli; continue; }
        if (eq(a, "-format")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            if (!set_output_mode(cli, OutputMode::Custom)) return cli;
            cli.format_template = v;
            continue;
        }

        if (eq(a, "-block-open")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.block_open = v;
            continue;
        }
        if (eq(a, "-block-close")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.block_close = v;
            continue;
        }

        if (eq(a, "-max-match-bytes")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.max_match_bytes = static_cast<uint64_t>(std::atoll(v));
            continue;
        }
        if (eq(a, "-max-context-bytes")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.max_context_bytes = static_cast<uint64_t>(std::atoll(v));
            continue;
        }
        if (eq(a, "-max-block-bytes")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.max_block_bytes = static_cast<uint64_t>(std::atoll(v));
            continue;
        }
        if (eq(a, "-max-output-bytes")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.max_output_bytes = static_cast<uint64_t>(std::atoll(v));
            continue;
        }

        if (eq(a, "-scope")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.scope_lang = v;
            continue;
        }
        if (eq(a, "-scope-pattern")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.scope_pattern = v;
            continue;
        }
        if (eq(a, "-scope-open")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.scope_open = v;
            continue;
        }
        if (eq(a, "-scope-close")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.scope_close = v;
            continue;
        }
        if (eq(a, "-scope-kind")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.scope_kind = v;
            continue;
        }

        if (eq(a, "-in-scope")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.in_scopes.emplace_back(v);
            continue;
        }
        if (eq(a, "-in-scope-kind")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.in_scope_kind = v;
            continue;
        }
        if (eq(a, "-lines")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            Cli::LineRange r;
            const char *colon = std::strchr(v, ':');
            int64_t lo = 0, hi = 0;
            bool ok = true;
            if (!colon) {
                ok = parse_nonneg(v, lo) && lo >= 1;   // "N"
                hi = lo;
            } else {
                std::string a_part(v, colon - v), b_part(colon + 1);
                if (a_part.empty() && b_part.empty()) ok = false;
                if (ok && !a_part.empty())
                    ok = parse_nonneg(a_part.c_str(), lo) && lo >= 1;
                if (ok && !b_part.empty())
                    ok = parse_nonneg(b_part.c_str(), hi) && hi >= 1;
                if (a_part.empty()) lo = 1;
                if (b_part.empty()) hi = 0;             // 0 = open end
                if (ok && hi != 0 && hi < lo) ok = false;
            }
            if (!ok) {
                cli.error = true;
                cli.error_message = std::string("-lines: expected N, A:B, "
                                                "A:, or :B (1-based) — got '") +
                                    v + "'";
                return cli;
            }
            r.lo = static_cast<uint32_t>(lo);
            r.hi = hi == 0 ? 0xffffffffu : static_cast<uint32_t>(hi);
            cli.line_ranges.push_back(r);
            continue;
        }
        if (eq(a, "-list-scopes") || eq(a, "--list-scopes")) {
            cli.list_scopes = true;
            continue;
        }

        if (eq(a, "-sample")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.sample_n = std::atoi(v);
            if (cli.sample_n < 0) cli.sample_n = 0;
            continue;
        }

        if (eq(a, "-near") || eq(a, "-far")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            // Parse A:B:K
            const char *colon1 = std::strchr(v, ':');
            if (!colon1) {
                cli.error = true;
                cli.error_message = std::string(a) + ": expected A:B:K";
                return cli;
            }
            const char *colon2 = std::strchr(colon1 + 1, ':');
            if (!colon2) {
                cli.error = true;
                cli.error_message = std::string(a) + ": expected A:B:K";
                return cli;
            }
            Cli::Relation r;
            r.kind = eq(a, "-near") ? Cli::RelationKind::Near
                                    : Cli::RelationKind::Far;
            r.a.assign(v, colon1 - v);
            r.b.assign(colon1 + 1, colon2 - colon1 - 1);
            r.lines = std::atoi(colon2 + 1);
            if (r.a.empty() || r.b.empty() || r.lines < 0) {
                cli.error = true;
                cli.error_message = std::string(a) + ": invalid A:B:K";
                return cli;
            }
            cli.relations.push_back(std::move(r));
            continue;
        }

        if (eq(a, "-same-scope") || eq(a, "-not-same-scope")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            // Parse A:B (no distance — containment, not proximity).
            const char *colon = std::strchr(v, ':');
            if (!colon || std::strchr(colon + 1, ':')) {
                cli.error = true;
                cli.error_message = std::string(a) + ": expected A:B";
                return cli;
            }
            Cli::Relation r;
            r.kind = eq(a, "-same-scope") ? Cli::RelationKind::SameScope
                                          : Cli::RelationKind::NotSameScope;
            r.a.assign(v, colon - v);
            r.b.assign(colon + 1);
            if (r.a.empty() || r.b.empty()) {
                cli.error = true;
                cli.error_message = std::string(a) + ": invalid A:B";
                return cli;
            }
            cli.relations.push_back(std::move(r));
            continue;
        }

        if (eq(a, "-summary") || eq(a, "--summary")) {
            cli.summary = true;
            continue;
        }
        if (eq(a, "-diagnostics") || eq(a, "--diagnostics")) {
            cli.diagnostics = true;
            continue;
        }
        if (eq(a, "-require-complete") || eq(a, "--require-complete")) {
            cli.require_complete = true;
            continue;
        }
        if (eq(a, "-records")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            if (eq(v, "line") || eq(v, "lines")) {
                cli.records = Cli::RecordMode::Line;
            } else {
                cli.error = true;
                cli.error_message = std::string("-records: unsupported mode '")
                                    + v + "' (supported: line)";
                return cli;
            }
            continue;
        }

        if (eq(a, "-s")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.script_inline = v;
            continue;
        }
        if (eq(a, "-script")) {
            const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
            cli.script_path = v;
            continue;
        }

        // Edit-subcommand flags. Only recognised after `hprscript edit` —
        // in search/script mode they fall through to "unknown flag", which
        // keeps the long-documented promise that bare hprscript rejects
        // write-shaped flags.
        if (cli.edit.active) {
            if (eq(a, "-span")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                if      (eq(v, "match"))      cli.edit.span = EditOptions::Span::Match;
                else if (eq(v, "line"))       cli.edit.span = EditOptions::Span::Line;
                else if (eq(v, "block"))      cli.edit.span = EditOptions::Span::Block;
                else if (eq(v, "block-full")) cli.edit.span = EditOptions::Span::BlockFull;
                else if (eq(v, "scope"))      cli.edit.span = EditOptions::Span::Scope;
                else if (eq(v, "scope-body")) cli.edit.span = EditOptions::Span::ScopeBody;
                else {
                    cli.error = true;
                    cli.error_message = std::string("-span: unknown span '") + v +
                        "' (match, line, block, block-full, scope, scope-body)";
                    return cli;
                }
                continue;
            }
            if (eq(a, "-content")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                cli.edit.content_set = true;
                cli.edit.content = v;
                continue;
            }
            if (eq(a, "-content-file")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                cli.edit.content_file = v;
                continue;
            }
            if (eq(a, "-content-stdin")) { cli.edit.content_stdin = true; continue; }
            if (eq(a, "-ref")) {
                if (cli.patterns.empty()) {
                    cli.error = true;
                    cli.error_message = "-ref must follow a -p/-pi/-F/-Fi";
                    return cli;
                }
                cli.patterns.back().ref = true;
                continue;
            }
            if (eq(a, "-insert")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                if (cli.edit.verb == EditOptions::Verb::Delete) {
                    cli.error = true;
                    cli.error_message = "-insert cannot combine with -delete";
                    return cli;
                }
                cli.edit.verb = EditOptions::Verb::Insert;
                if      (eq(v, "before")) cli.edit.insert_pos = EditOptions::InsertPos::Before;
                else if (eq(v, "after"))  cli.edit.insert_pos = EditOptions::InsertPos::After;
                else if (eq(v, "start"))  cli.edit.insert_pos = EditOptions::InsertPos::Start;
                else if (eq(v, "end"))    cli.edit.insert_pos = EditOptions::InsertPos::End;
                else {
                    cli.error = true;
                    cli.error_message = std::string("-insert: unknown position '")
                                        + v + "' (before, after, start, end)";
                    return cli;
                }
                continue;
            }
            if (eq(a, "-delete")) {
                if (cli.edit.verb == EditOptions::Verb::Insert) {
                    cli.error = true;
                    cli.error_message = "-delete cannot combine with -insert";
                    return cli;
                }
                cli.edit.verb = EditOptions::Verb::Delete;
                continue;
            }
            if (eq(a, "-write"))  { cli.edit.write = true; continue; }
            if (eq(a, "-diff"))   { cli.edit.diff = true; continue; }
            if (eq(a, "-expect")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                if (!parse_nonneg(v, cli.edit.expect)) {
                    cli.error = true;
                    cli.error_message =
                        std::string("-expect: not a non-negative integer: ") + v;
                    return cli;
                }
                continue;
            }
            if (eq(a, "-max-span-lines")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                if (!parse_nonneg(v, cli.edit.max_span_lines)) {
                    cli.error = true;
                    cli.error_message =
                        std::string("-max-span-lines: not a non-negative integer: ") + v;
                    return cli;
                }
                continue;
            }
            if (eq(a, "-assert-contains")) {
                const char *v = take(i, argc, argv, a, cli); if (!v) return cli;
                cli.edit.assert_contains = v;
                continue;
            }
        }

        // `--` ends flag parsing; the rest is positional. Documented for
        // -script usage: `hprscript -script find_funcs.hpr -- src/foo.go`.
        if (eq(a, "--")) {
            for (++i; i < argc; ++i) cli.positional.emplace_back(argv[i]);
            break;
        }

        // Unknown flag (starts with '-' but isn't bare '-' for stdin).
        if (a[0] == '-' && a[1] != '\0') {
            cli.error = true;
            cli.error_message = std::string("unknown flag: ") + a;
            return cli;
        }
        cli.positional.emplace_back(a);
    }

    if (!cli.patterns.empty() &&
        (!cli.script_inline.empty() || !cli.script_path.empty())) {
        cli.error = true;
        cli.error_message = "-p/-pi cannot be combined with -s or -script";
    }
    if (cli.git_added_lines && !cli.git_changed && !cli.git_staged &&
        cli.git_ranges.empty()) {
        cli.error = true;
        cli.error_message = "-git-added-lines requires -git-changed, "
                            "-git-staged, or -git-range";
        return cli;
    }
    int stdin_lists = 0;
    for (const auto &fl : cli.file_lists) if (fl.path == "-") ++stdin_lists;
    if (stdin_lists > 1) {
        cli.error = true;
        cli.error_message =
            "only one -files-from/-files0-from may read from stdin";
    }
    if (!cli.error && !cli.show_help && !cli.show_version) {
        const bool script_mode =
            !cli.script_inline.empty() || !cli.script_path.empty();
        if (script_mode &&
            (!cli.in_scopes.empty() || !cli.in_scope_kind.empty() ||
             !cli.line_ranges.empty())) {
            cli.error = true;
            cli.error_message = "-in-scope/-in-scope-kind/-lines work in "
                                "quick (-p) and edit modes only";
            return cli;
        }
        if (cli.list_scopes) {
            if (cli.edit.active) {
                cli.error = true;
                cli.error_message = "-list-scopes cannot combine with edit mode";
            } else if (!cli.patterns.empty() || !cli.patterns_from.empty()) {
                cli.error = true;
                cli.error_message = "-list-scopes takes no patterns — it "
                                    "lists the scope index itself";
            } else if (script_mode) {
                cli.error = true;
                cli.error_message = "-list-scopes cannot combine with -s/-script";
            } else if (cli.out_mode_set &&
                       cli.out_mode != OutputMode::JsonLines &&
                       cli.out_mode != OutputMode::Llm) {
                cli.error = true;
                cli.error_message =
                    "-list-scopes output is JSONL (default) or -llm";
            } else if (cli.sample_n > 0 ||
                       cli.records != Cli::RecordMode::None) {
                cli.error = true;
                cli.error_message =
                    "-list-scopes cannot combine with -sample/-records";
            } else if (!cli.line_ranges.empty()) {
                cli.error = true;
                cli.error_message = "-list-scopes lists whole scopes — "
                                    "-lines does not apply";
            }
            if (cli.error) return cli;
        }
    }
    if (cli.edit.active && !cli.error && !cli.show_help && !cli.show_version)
        validate_edit_cli(cli);
    return cli;
}

bool load_patterns_from(Cli &cli) {
    for (const auto &path : cli.patterns_from) {
        std::ifstream in(path);
        if (!in) {
            cli.error = true;
            cli.error_message = "cannot read patterns file: " + path;
            return false;
        }
        std::string line;
        size_t lineno = 0;
        auto fail = [&](const std::string &msg) {
            cli.error = true;
            cli.error_message =
                path + ":" + std::to_string(lineno) + ": " + msg;
            return false;
        };
        while (std::getline(in, line)) {
            ++lineno;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t ws = line.find_first_not_of(" \t");
            if (ws == std::string::npos) continue;   // blank line
            if (line[ws] == '#') continue;           // comment
            auto pr = json::parse(line);
            if (!pr.ok) return fail("JSON parse error: " + pr.error);
            if (!pr.value.is_object())
                return fail("pattern entry must be a JSON object");
            for (const auto &kv : pr.value.as_object()) {
                if (kv.first != "id" && kv.first != "regexp" &&
                    kv.first != "literal" && kv.first != "case_insensitive" &&
                    kv.first != "word_boundary" && kv.first != "utf8" &&
                    kv.first != "ref")
                    return fail("unknown field '" + kv.first + "'");
            }
            const json::Value *re = pr.value.find("regexp");
            const json::Value *lit = pr.value.find("literal");
            if ((re && lit) || (!re && !lit))
                return fail("need exactly one of 'regexp' or 'literal'");
            CliPattern p;
            if (re) {
                if (!re->is_string()) return fail("'regexp' must be a string");
                p.regexp = re->as_string();
            } else {
                if (!lit->is_string()) return fail("'literal' must be a string");
                p.regexp = escape_literal(lit->as_string().c_str());
            }
            if (const json::Value *v = pr.value.find("id")) {
                if (!v->is_string() ||
                    !valid_pattern_name(v->as_string().c_str()))
                    return fail("'id' must be an identifier "
                                "([A-Za-z_][A-Za-z0-9_]*)");
                p.name = v->as_string();
            }
            if (const json::Value *v = pr.value.find("case_insensitive")) {
                if (!v->is_bool())
                    return fail("'case_insensitive' must be a boolean");
                p.case_insensitive = v->as_bool();
            }
            if (const json::Value *v = pr.value.find("word_boundary")) {
                if (!v->is_bool())
                    return fail("'word_boundary' must be a boolean");
                p.word_boundary = v->as_bool() ? 1 : 0;
            }
            if (const json::Value *v = pr.value.find("utf8")) {
                if (!v->is_bool()) return fail("'utf8' must be a boolean");
                p.utf8 = v->as_bool() ? 1 : 0;
            }
            if (const json::Value *v = pr.value.find("ref")) {
                if (!v->is_bool()) return fail("'ref' must be a boolean");
                p.ref = v->as_bool();
            }
            cli.patterns.push_back(std::move(p));
        }
    }
    return true;
}

// Final pattern ids must be unique — a `-name`d pattern may otherwise
// collide with another pattern's name or auto `p<i>` id, making relations
// and `pat` attribution ambiguous. Called after -patterns-from loading.
bool validate_pattern_ids(Cli &cli) {
    std::vector<std::string> ids;
    ids.reserve(cli.patterns.size());
    for (size_t i = 0; i < cli.patterns.size(); ++i) {
        const std::string &n = cli.patterns[i].name;
        ids.push_back(n.empty() ? "p" + std::to_string(i) : n);
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        for (size_t j = i + 1; j < ids.size(); ++j) {
            if (ids[i] == ids[j]) {
                cli.error = true;
                cli.error_message = "duplicate pattern id '" + ids[i] + "'";
                return false;
            }
        }
    }
    return true;
}

} // namespace hpr
