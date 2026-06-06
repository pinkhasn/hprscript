#include "cli.hpp"

#include <cstdlib>
#include <cstring>

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
"\n"
"Search flags (with -p):\n"
"  -p <pattern>     Search pattern (PCRE; repeatable for multi-pattern)\n"
"  -pi <pattern>    Case-insensitive search pattern (repeatable)\n"
"  -extract n1,n2,…  Re-extract capture groups from the preceding -p/-pi\n"
"  -glob <glob>     Scan glob (e.g. \"**/*.go\"; repeatable)\n"
"  -exclude <pat>   Exclude rule: glob, bare dir name, or path prefix (repeatable)\n"
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
"Pattern relations (filter matches by proximity, repeatable):\n"
"  -near A:B:K      Emit pattern A's matches with a B-match within K lines\n"
"  -far  A:B:K      Emit A's matches with NO B-match within K lines (K=0=same)\n"
"\n"
"Enclosing-scope annotation (with -p):\n"
"  -scope <lang|auto>       Built-in pack (auto, go, rust, c, cpp, java, js, ts)\n"
"  -scope-pattern <regex>   Custom scope-anchor regex (capture group 1 = name)\n"
"  -scope-open <s>          Body opening delimiter for the custom anchor\n"
"  -scope-close <s>         Body closing delimiter\n"
"  -scope-kind <s>          Label emitted as enclosing.kind (default 'func')\n"
"\n"
"Script mode:\n"
"  -s <json>        Inline script\n"
"  -script <path>   Script file\n"
"\n"
"Misc:\n"
"  --version        Show version\n"
"  -h, --help       This help\n");
}

Cli parse_cli(int argc, char **argv) {
    Cli cli;
    for (int i = 1; i < argc; ++i) {
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
    return cli;
}

} // namespace hpr
