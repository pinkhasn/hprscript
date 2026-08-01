// The `edit` subcommand engine.
//
// The contract (HPRSCRIPT.md, "Edit mode" chapter) in one paragraph:
// targeting reuses the exact search pipeline (patterns, relations,
// -file-where, git filters), each
// surviving match resolves to a byte span (match / line / block /
// block-full), spans become splices (replace / insert / delete), and EVERY
// guard is validated across ALL files before a single byte is written.
// Dry-run is the default and prints a unified diff computed from the same
// splice plan that -write applies; guard violations exit 3 with nothing
// written. Splices are byte-exact: CRLF, missing trailing newlines, and
// non-UTF-8 bytes pass through untouched.
//
// Two-phase structure:
//   plan:  scan files -> kept matches -> resolve spans -> expand content ->
//          per-site guards -> sort/dedup -> overlap conflicts -> -expect
//   apply: (only with -write, only if zero violations) drift-check every
//          file, then per file: splice -> temp file -> fchmod -> rename.

#include "edit.hpp"

#include "block.hpp"
#include "extract.hpp"
#include "file_io.hpp"
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
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace hpr {
namespace {

// glibc (Linux) names `struct stat`'s POSIX nanosecond mtime field
// `st_mtim`; BSD/Darwin (macOS) names the same struct timespec
// `st_mtimespec`. Centralize the one platform difference here instead of
// scattering #ifdefs at each read site.
#if defined(__APPLE__)
struct timespec stat_mtime(const struct stat &st) { return st.st_mtimespec; }
#else
struct timespec stat_mtime(const struct stat &st) { return st.st_mtim; }
#endif

// One planned splice: bytes [start,end) of the file are replaced by `text`.
// Inserts are zero-width (start == end). `span_*` is the resolved span the
// site was derived from — for replace/delete it equals [start,end); for
// insert it is the anchor span the position was computed from (guards like
// -assert-contains and -max-span-lines run against the span).
struct Site {
    uint64_t start = 0, end = 0;
    std::string text;
    uint64_t span_start = 0, span_end = 0;
    std::string pat;
    uint32_t line_start = 1, line_end = 1; // 1-based lines of [start,end)
    bool noop = false;
};

struct PlanFile {
    std::string path;       // as walked; used in records and diffs
    std::string write_path; // resolved target (differs when path is a symlink)
    std::string content;    // owned copy of the original bytes
    LineIndex idx;          // built over `content`
    std::vector<Site> sites;
    // Plan-time stat of the write target, for the drift check and fchmod.
    off_t st_size = 0;
    struct timespec st_mtim = {};
    mode_t st_mode = 0644;
    bool symlink = false;
};

struct Violation {
    std::string guard; // overlap | expect | max-span-lines | assert-contains
                       // | block-not-found | changed-during-run
    std::string file;  // empty when not file-specific (expect)
    uint32_t line = 0; // 0 = n/a
    std::string message;
};

const char *span_name(EditOptions::Span s) {
    switch (s) {
        case EditOptions::Span::Match:     return "match";
        case EditOptions::Span::Line:      return "line";
        case EditOptions::Span::Block:     return "block";
        case EditOptions::Span::BlockFull: return "block-full";
        case EditOptions::Span::Scope:     return "scope";
        case EditOptions::Span::ScopeBody: return "scope-body";
    }
    return "?";
}

const char *verb_name(EditOptions::Verb v) {
    switch (v) {
        case EditOptions::Verb::Replace: return "replace";
        case EditOptions::Verb::Insert:  return "insert";
        case EditOptions::Verb::Delete:  return "delete";
    }
    return "?";
}

// ---- JSON record emission ---------------------------------------------------

void emit_guard_record(const Violation &v) {
    std::string s = "{\"type\":\"guard\",\"guard\":\"";
    json_escape_to(s, v.guard);
    s += "\"";
    if (!v.file.empty()) {
        s += ",\"file\":\"";
        json_escape_to(s, v.file);
        s += "\"";
    }
    if (v.line > 0) s += ",\"line\":" + std::to_string(v.line);
    s += ",\"message\":\"";
    json_escape_to(s, v.message);
    s += "\"}\n";
    std::fputs(s.c_str(), stdout);
}

void emit_edit_record(const PlanFile &pf, const Site &st, const Cli &cli) {
    std::string s = "{\"type\":\"edit\",\"file\":\"";
    json_escape_to(s, pf.path);
    s += "\",\"pat\":\"";
    json_escape_to(s, st.pat);
    s += "\",\"verb\":\"";
    s += verb_name(cli.edit.verb);
    s += "\",\"span\":\"";
    s += span_name(cli.edit.span);
    s += "\",\"line_start\":" + std::to_string(st.line_start);
    s += ",\"line_end\":" + std::to_string(st.line_end);
    s += ",\"bytes_removed\":" + std::to_string(st.noop ? 0 : st.end - st.start);
    s += ",\"bytes_added\":" +
         std::to_string(st.noop ? 0 : (uint64_t)st.text.size());
    s += ",\"status\":\"";
    s += st.noop ? "noop" : "changed";
    if (pf.symlink) {
        s += "\",\"symlink_target\":\"";
        json_escape_to(s, pf.write_path);
    }
    s += "\"}\n";
    std::fputs(s.c_str(), stdout);
}

void emit_edit_summary(uint64_t sites, uint64_t changed, uint64_t noops,
                       uint64_t files_changed, bool dry_run, bool applied) {
    std::string s = "{\"type\":\"edit-summary\",\"sites\":" +
                    std::to_string(sites);
    s += ",\"changed\":" + std::to_string(changed);
    s += ",\"noops\":" + std::to_string(noops);
    s += ",\"files_changed\":" + std::to_string(files_changed);
    s += std::string(",\"dry_run\":") + (dry_run ? "true" : "false");
    s += std::string(",\"applied\":") + (applied ? "true" : "false");
    s += "}\n";
    std::fputs(s.c_str(), stdout);
}

// ---- content templates --------------------------------------------------

// Interpret backslash escapes in an inline -content template exactly once:
// \n \t \r \\ . Anything else keeps the backslash (so Go/C escapes inside
// pasted code don't need doubling unless they collide with these four).
// -content-file / -content-stdin bytes are used verbatim and never pass
// through here.
std::string unescape_content(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) {
            out += in[i];
            continue;
        }
        switch (in[i + 1]) {
            case 'n': out += '\n'; ++i; break;
            case 't': out += '\t'; ++i; break;
            case 'r': out += '\r'; ++i; break;
            case '\\': out += '\\'; ++i; break;
            default: out += '\\'; break;
        }
    }
    return out;
}

struct TokenCtx {
    std::string_view match_text;
    const std::string *file = nullptr;
    uint32_t line = 0;
    const std::string *pat_id = nullptr;
    const std::vector<std::string> *extract_names = nullptr;
    const std::vector<std::string> *extract_values = nullptr;
    const ScopeRange *enclosing = nullptr;
};

bool token_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i]))
            return false;
    return true;
}

// Expand $-tokens in a -content template: $$ -> literal $, $MATCH, $FILE,
// $LINE, $PAT_ID, $EXTRACT_<NAME> (case-insensitive against the pattern's
// -extract names), $ENCLOSING_NAME / $ENCLOSING_KIND when -scope is active.
// Unrecognised $tokens are kept literally, matching -format behavior.
std::string expand_template(const std::string &tmpl, const TokenCtx &ctx) {
    std::string out;
    out.reserve(tmpl.size() + 16);
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] != '$') {
            out += tmpl[i++];
            continue;
        }
        if (i + 1 < tmpl.size() && tmpl[i + 1] == '$') {
            out += '$';
            i += 2;
            continue;
        }
        // Read the identifier after '$'.
        size_t j = i + 1;
        while (j < tmpl.size() &&
               (std::isalnum((unsigned char)tmpl[j]) || tmpl[j] == '_'))
            ++j;
        std::string_view tok(tmpl.data() + i + 1, j - i - 1);
        bool handled = true;
        if (tok == "MATCH") {
            out.append(ctx.match_text.data(), ctx.match_text.size());
        } else if (tok == "FILE") {
            out += *ctx.file;
        } else if (tok == "LINE") {
            out += std::to_string(ctx.line);
        } else if (tok == "PAT_ID") {
            out += *ctx.pat_id;
        } else if (tok == "ENCLOSING_NAME") {
            if (ctx.enclosing) out += ctx.enclosing->name;
        } else if (tok == "ENCLOSING_KIND") {
            if (ctx.enclosing) out += ctx.enclosing->kind;
        } else if (tok.size() > 8 && tok.substr(0, 8) == "EXTRACT_" &&
                   ctx.extract_names) {
            std::string_view want = tok.substr(8);
            bool found = false;
            for (size_t k = 0; k < ctx.extract_names->size(); ++k) {
                if (token_ieq((*ctx.extract_names)[k], want)) {
                    if (ctx.extract_values && k < ctx.extract_values->size())
                        out += (*ctx.extract_values)[k];
                    found = true;
                    break;
                }
            }
            handled = found;
        } else {
            handled = false;
        }
        if (handled) {
            i = j;
        } else {
            out += '$';
            ++i;
        }
    }
    return out;
}

// ---- unified diff -----------------------------------------------------------
//
// No diff algorithm needed: hunks derive directly from the splice plan.
// Each non-noop site expands to the full old lines it touches; sites whose
// line windows overlap merge into one group; groups plus 3 context lines
// merge into hunks when their contexts meet. New-file line numbers carry
// the cumulative line delta of earlier hunks.

bool ends_with_nl(std::string_view s) { return !s.empty() && s.back() == '\n'; }

// Lines that actually exist for diff purposes: the phantom empty line after
// a trailing '\n' does not count, and an empty file has zero lines.
uint32_t real_line_count(const PlanFile &pf) {
    if (pf.content.empty()) return 0;
    return pf.idx.line_count() - (ends_with_nl(pf.content) ? 1 : 0);
}

// A pure line insertion sits at a line boundary and inserts only whole
// lines (text ends with '\n', or the insertion point is EOF). Everything
// else — mid-line splices, inserts joining a neighbour line — is expressed
// by rewriting the touched line(s).
bool is_pure_line_insert(const PlanFile &pf, const Site &st) {
    if (st.start != st.end || st.text.empty()) return false;
    bool at_line_start = st.start == 0 || pf.content[st.start - 1] == '\n';
    if (!at_line_start) return false;
    return ends_with_nl(st.text) || st.start == pf.content.size();
}

void split_lines(std::string_view text, std::vector<std::string> &out) {
    out.clear();
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            out.emplace_back(text.substr(pos));
            break;
        }
        out.emplace_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

// A group of sites whose old-line windows overlap. old_hi < old_lo means an
// empty old window (pure insertion before old_lo).
struct DiffGroup {
    uint32_t old_lo = 1;
    uint32_t old_hi = 0;
    std::vector<const Site *> sites; // byte order
};

// Byte range [wb,we) of old lines lo..hi (inclusive of the trailing '\n' of
// line hi when present). Empty window -> both at the line-start of lo.
void group_window(const PlanFile &pf, const DiffGroup &g, uint64_t &wb,
                  uint64_t &we) {
    if (g.old_hi < g.old_lo) {
        wb = we = g.sites.front()->start;
        return;
    }
    uint64_t s, e;
    pf.idx.line_range(pf.idx.line_text(g.old_lo).data() - pf.content.data(),
                      s, e);
    wb = s;
    std::string_view last = pf.idx.line_text(g.old_hi);
    uint64_t last_end = (last.data() - pf.content.data()) + last.size();
    we = last_end < pf.content.size() && pf.content[last_end] == '\n'
             ? last_end + 1
             : last_end;
}

std::string splice_range(const PlanFile &pf, const DiffGroup &g, uint64_t wb,
                         uint64_t we) {
    std::string out;
    uint64_t cur = wb;
    for (const Site *st : g.sites) {
        out.append(pf.content, cur, st->start - cur);
        out += st->text;
        cur = st->end;
    }
    out.append(pf.content, cur, we - cur);
    return out;
}

// Print the unified diff for one file's non-noop sites. Returns the total
// line delta (new lines - old lines), which callers don't currently need
// but keeps the accounting explicit.
void print_file_diff(const PlanFile &pf, FILE *out) {
    std::vector<const Site *> live;
    for (const Site &st : pf.sites)
        if (!st.noop) live.push_back(&st);
    if (live.empty()) return;

    const uint32_t real_lines = real_line_count(pf);
    const bool old_nl = ends_with_nl(pf.content) || pf.content.empty();

    // Build groups of sites with overlapping line windows.
    std::vector<DiffGroup> groups;
    for (const Site *st : live) {
        uint32_t lo, hi;
        if (is_pure_line_insert(pf, *st)) {
            lo = pf.idx.line_of(st->start);
            hi = lo - 1; // empty window: insert before line `lo`
        } else {
            lo = pf.idx.line_of(st->start);
            hi = pf.idx.line_of(st->end > st->start ? st->end - 1 : st->start);
            if (hi > real_lines && real_lines > 0) hi = real_lines;
            if (lo > real_lines && real_lines > 0) lo = real_lines;
        }
        if (!groups.empty()) {
            DiffGroup &cur = groups.back();
            uint32_t cur_eff_hi =
                std::max(cur.old_hi, cur.old_lo > 0 ? cur.old_lo - 1 : 0u);
            if (lo <= cur_eff_hi) {
                // Overlapping windows: extend the current group.
                cur.old_lo = std::min(cur.old_lo, lo);
                cur.old_hi = std::max(cur.old_hi, hi);
                cur.sites.push_back(st);
                continue;
            }
        }
        DiffGroup g;
        g.old_lo = lo;
        g.old_hi = hi;
        g.sites.push_back(st);
        groups.push_back(std::move(g));
    }

    // Per group: new lines + whether the new window ends the file without a
    // trailing newline.
    struct Rendered {
        const DiffGroup *g;
        std::vector<std::string> new_lines;
        bool new_eof_no_nl = false; // group reaches EOF and new text lacks \n
    };
    std::vector<Rendered> rendered;
    rendered.reserve(groups.size());
    for (const DiffGroup &g : groups) {
        uint64_t wb, we;
        group_window(pf, g, wb, we);
        std::string nt = splice_range(pf, g, wb, we);
        Rendered r;
        r.g = &g;
        split_lines(nt, r.new_lines);
        r.new_eof_no_nl = we >= pf.content.size() && !nt.empty() &&
                          nt.back() != '\n';
        rendered.push_back(std::move(r));
    }

    // Merge groups into hunks when their 3-line contexts meet, then print.
    const uint32_t CTX = 3;
    std::fprintf(out, "--- a/%s\n+++ b/%s\n", pf.path.c_str(),
                 pf.path.c_str());
    int64_t delta = 0; // cumulative (new - old) line delta from prior hunks
    size_t gi = 0;
    while (gi < rendered.size()) {
        // Hunk = groups [gi, gj)
        size_t gj = gi + 1;
        while (gj < rendered.size()) {
            uint32_t prev_hi = std::max(rendered[gj - 1].g->old_hi,
                                        rendered[gj - 1].g->old_lo - 1);
            uint32_t next_lo = rendered[gj].g->old_lo;
            if (next_lo > prev_hi + 2 * CTX + 1) break;
            ++gj;
        }
        uint32_t first_lo = rendered[gi].g->old_lo;
        uint32_t last_hi = std::max(rendered[gj - 1].g->old_hi,
                                    rendered[gj - 1].g->old_lo - 1);
        uint32_t ctx_lo = first_lo > CTX ? first_lo - CTX : 1;
        uint32_t ctx_hi = std::min(real_lines, last_hi + CTX);

        // Counts.
        uint32_t old_count = ctx_hi >= ctx_lo ? ctx_hi - ctx_lo + 1 : 0;
        int64_t new_count = old_count;
        for (size_t k = gi; k < gj; ++k) {
            uint32_t olines = rendered[k].g->old_hi >= rendered[k].g->old_lo
                                  ? rendered[k].g->old_hi -
                                        rendered[k].g->old_lo + 1
                                  : 0;
            new_count += (int64_t)rendered[k].new_lines.size() - olines;
        }
        int64_t new_start =
            old_count == 0 ? (int64_t)ctx_lo - 1 + delta + 1
                           : (int64_t)ctx_lo + delta;
        std::fprintf(out, "@@ -%u,%u +%lld,%lld @@\n",
                     old_count == 0 ? ctx_lo - 1 : ctx_lo, old_count,
                     (long long)(new_count == 0 ? new_start - 1 : new_start),
                     (long long)new_count);

        auto put_old = [&](uint32_t L, char prefix) {
            std::string_view t = pf.idx.line_text(L);
            std::fputc(prefix, out);
            std::fwrite(t.data(), 1, t.size(), out);
            std::fputc('\n', out);
            if (L == real_lines && !old_nl)
                std::fputs("\\ No newline at end of file\n", out);
        };

        uint32_t cursor = ctx_lo;
        for (size_t k = gi; k < gj; ++k) {
            const Rendered &r = rendered[k];
            for (; cursor < r.g->old_lo; ++cursor) put_old(cursor, ' ');
            if (r.g->old_hi >= r.g->old_lo)
                for (; cursor <= r.g->old_hi; ++cursor) put_old(cursor, '-');
            for (size_t li = 0; li < r.new_lines.size(); ++li) {
                std::fputc('+', out);
                std::fwrite(r.new_lines[li].data(), 1, r.new_lines[li].size(),
                            out);
                std::fputc('\n', out);
                if (li + 1 == r.new_lines.size() && r.new_eof_no_nl)
                    std::fputs("\\ No newline at end of file\n", out);
            }
        }
        for (; cursor <= ctx_hi; ++cursor) put_old(cursor, ' ');

        for (size_t k = gi; k < gj; ++k) {
            uint32_t olines = rendered[k].g->old_hi >= rendered[k].g->old_lo
                                  ? rendered[k].g->old_hi -
                                        rendered[k].g->old_lo + 1
                                  : 0;
            delta += (int64_t)rendered[k].new_lines.size() - olines;
        }
        gi = gj;
    }
}

// ---- apply --------------------------------------------------------------

std::string build_new_content(const PlanFile &pf) {
    std::string out;
    out.reserve(pf.content.size() + 256);
    uint64_t cur = 0;
    for (const Site &st : pf.sites) {
        if (st.noop) continue;
        out.append(pf.content, cur, st.start - cur);
        out += st.text;
        cur = st.end;
    }
    out.append(pf.content, cur, pf.content.size() - cur);
    return out;
}

// Atomic write: temp file in the same directory, fchmod to the original
// mode, fsync, rename over the target. Returns false with `err` set.
bool write_file_atomic(const PlanFile &pf, const std::string &data,
                       std::string &err) {
    std::filesystem::path target(pf.write_path);
    std::string tmpl = (target.parent_path().empty()
                            ? std::filesystem::path(".")
                            : target.parent_path())
                           .string() +
                       "/.hpr-edit." + target.filename().string() + ".XXXXXX";
    std::vector<char> tmp(tmpl.begin(), tmpl.end());
    tmp.push_back('\0');
    int fd = ::mkstemp(tmp.data());
    if (fd < 0) {
        err = std::string("cannot create temp file near ") + pf.write_path +
              ": " + std::strerror(errno);
        return false;
    }
    std::string tmp_path(tmp.data());
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = std::string("write failed for ") + tmp_path + ": " +
                  std::strerror(errno);
            ok = false;
            break;
        }
        off += (size_t)n;
    }
    if (ok && ::fchmod(fd, pf.st_mode) != 0) {
        err = std::string("fchmod failed for ") + tmp_path + ": " +
              std::strerror(errno);
        ok = false;
    }
    if (ok && ::fsync(fd) != 0) {
        err = std::string("fsync failed for ") + tmp_path + ": " +
              std::strerror(errno);
        ok = false;
    }
    ::close(fd);
    if (ok && std::rename(tmp_path.c_str(), pf.write_path.c_str()) != 0) {
        err = std::string("rename failed for ") + pf.write_path + ": " +
              std::strerror(errno);
        ok = false;
    }
    if (!ok) ::unlink(tmp_path.c_str());
    return ok;
}

} // namespace

int run_edit(const Cli &cli) {
    const EditOptions &ed = cli.edit;
    TargetFilter tf;
    if (!tf.init(cli)) return 2;
    const bool scope_span = ed.span == EditOptions::Span::Scope ||
                            ed.span == EditOptions::Span::ScopeBody;
    // Anchorless mode: no -p at all — edit sites come straight from scopes
    // matching -in-scope/-in-scope-kind ("replace the function named X").
    const bool anchorless = cli.patterns.empty();
    if (anchorless && !(tf.scope_needed() && scope_span)) {
        std::fprintf(stderr,
                     "hprscript: edit: -p <pattern> required (anchorless "
                     "edits need -in-scope/-in-scope-kind together with "
                     "-span scope|scope-body)\n");
        return 2;
    }
    const auto t_start = std::chrono::steady_clock::now();
    ScanStats stats;

    // ---- setup: identical targeting machinery to search mode ----
    std::vector<Pattern> patterns = build_patterns(cli);
    if (!anchorless) {
        // Re-checked here because -patterns-from packs load after parse-time
        // validation: an all-ref pattern set has nothing to edit.
        bool any_target = false;
        for (const auto &p : patterns)
            if (!p.ref) any_target = true;
        if (!any_target) {
            std::fprintf(stderr,
                         "hprscript: every pattern is -ref (reference-only); "
                         "at least one pattern must produce edits\n");
            return 2;
        }
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

    Matcher matcher;
    if (!anchorless) {
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
    }
    ExtractTable extract_table;
    {
        std::string ee;
        int ee_idx = -1;
        if (!extract_table.build(patterns, &ee, &ee_idx)) {
            std::fprintf(stderr, "hprscript: %s\n", ee.c_str());
            return 2;
        }
    }

    if (!cli.scope_lang.empty() && cli.scope_lang != "auto" &&
        !builtin_scope_pack(cli.scope_lang)) {
        std::fprintf(stderr,
                     "hprscript: unknown -scope pack '%s' (supported: auto, "
                     "go, rust, c, cpp, java, js, ts)\n",
                     cli.scope_lang.c_str());
        return 2;
    }
    ScopeConfig user_scope_custom;
    user_scope_custom.anchor_regex = cli.scope_pattern;
    user_scope_custom.open = cli.scope_open;
    user_scope_custom.close = cli.scope_close;
    user_scope_custom.kind = cli.scope_kind;
    std::string eff_scope_lang = cli.scope_lang;
    if (tf.scope_needed() && eff_scope_lang.empty() &&
        cli.scope_pattern.empty())
        eff_scope_lang = "auto";
    const bool scope_enabled =
        !eff_scope_lang.empty() ||
        (!cli.scope_pattern.empty() && !cli.scope_open.empty() &&
         !cli.scope_close.empty());
    if (any_scope_rel && !scope_enabled) {
        std::fprintf(stderr,
                     "hprscript: -same-scope/-not-same-scope require an "
                     "active -scope (built-in pack or -scope-pattern)\n");
        return 2;
    }

    // ---- content source ----
    std::string body;             // template text or verbatim bytes
    bool body_is_template = false;
    if (ed.content_set) {
        body = unescape_content(ed.content);
        body_is_template = body.find('$') != std::string::npos;
    } else if (!ed.content_file.empty()) {
        std::ifstream in(ed.content_file, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "hprscript: edit: cannot read %s\n",
                         ed.content_file.c_str());
            return 2;
        }
        std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        body = std::move(data);
    } else if (ed.content_stdin) {
        if (!read_stdin(body)) {
            std::fprintf(stderr, "hprscript: edit: failed to read stdin\n");
            return 2;
        }
    }

    // -assert-contains compiles with the same engine as -extract
    // (ECMAScript std::regex), search semantics over the resolved span.
    std::regex assert_re;
    if (!ed.assert_contains.empty()) {
        try {
            assert_re.assign(ed.assert_contains, std::regex::ECMAScript);
        } catch (const std::regex_error &e) {
            std::fprintf(stderr, "hprscript: -assert-contains: %s\n",
                         e.what());
            return 2;
        }
    }

    Walker walker;
    std::unordered_map<std::string, AddedLines> added;
    if (!add_walker_inputs(cli, walker, stats, added)) return 2;

    MatchCollector collector(patterns, std::move(rels), cli.git_added_lines);

    // ---- plan pass ----
    std::vector<PlanFile> files;
    std::vector<Violation> violations;
    uint64_t total_matches = 0;
    bool io_error = false;

    walker.walk([&](const WalkItem &it) -> bool {
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
        std::string_view content = mf.view();

        LineIndex idx;
        idx.build(content);
        ScopeIndex scope;
        ScopeConfig scope_cfg;
        const ScopeIndex *scope_ptr = nullptr;
        if (scope_enabled) {
            scope_ptr = build_file_scope(eff_scope_lang, user_scope_custom,
                                         it.path, content, idx, scope,
                                         &scope_cfg);
        }

        bool stop_walk = false;
        std::vector<Match> kept;
        if (!anchorless) {
            const AddedLines *al = nullptr;
            if (cli.git_added_lines) {
                auto ait = added.find(it.path);
                if (ait != added.end()) al = &ait->second;
            }
            collector.collect(matcher, content, idx, scope_ptr, al, kept);
            tf.apply(kept, idx, scope_ptr);
            if (kept.empty()) return true;
            if (fw.active() &&
                !fw.pass(kept, patterns.size(), it.path, churn_map))
                return true;
            stats.matches_seen += kept.size();

            // -ref patterns qualify (relations above, -file-where just now)
            // but never edit — drop their matches before limits & span work.
            kept.erase(std::remove_if(kept.begin(), kept.end(),
                                      [&](const Match &m) {
                                          return patterns[m.pattern_index].ref;
                                      }),
                       kept.end());
            if (kept.empty()) return true;

            // -m / -limit bound the matches considered (documented: prefer
            // -expect for exactness — limits truncate silently by design).
            if (cli.per_file_limit > 0 &&
                kept.size() > (uint64_t)cli.per_file_limit)
                kept.resize(cli.per_file_limit);
            if (cli.limit > 0) {
                uint64_t room = (uint64_t)cli.limit > total_matches
                                    ? cli.limit - total_matches
                                    : 0;
                if (kept.size() >= room) {
                    kept.resize(room);
                    stop_walk = true;
                    if (stats.stop_reason.empty()) stats.stop_reason = "limit";
                }
            }
            total_matches += kept.size();
            if (kept.empty()) return !stop_walk;
        } else {
            // Anchorless: skip the copy below unless some scope matches.
            if (!scope_ptr) return true;
            bool any = false;
            for (const ScopeRange &r : scope_ptr->all())
                if (tf.scope_matches(r)) { any = true; break; }
            if (!any) return true;
        }

        PlanFile pf;
        pf.path = it.path;
        pf.content.assign(content.data(), content.size());
        pf.idx.build(pf.content);

        // Plan-time stat of the write target (drift check + mode).
        std::error_code ec;
        pf.symlink = std::filesystem::is_symlink(it.path, ec);
        pf.write_path = pf.symlink
                            ? std::filesystem::canonical(it.path, ec).string()
                            : it.path;
        if (ec) pf.write_path = it.path;
        struct stat stbuf;
        if (::stat(pf.write_path.c_str(), &stbuf) == 0) {
            pf.st_size = stbuf.st_size;
            pf.st_mtim = stat_mtime(stbuf);
            pf.st_mode = stbuf.st_mode & 07777;
        }

        // Innermost containing scope for span targeting. With an active
        // -in-scope filter, "innermost" means the innermost scope that
        // SATISFIES the filter — `-p retry -in-scope ProcessBatch -span
        // scope` must target ProcessBatch, not a closure the match sits in.
        auto innermost_span_scope = [&](uint64_t off) -> const ScopeRange * {
            if (!scope_ptr) return nullptr;
            const ScopeRange *best = nullptr;
            for (const ScopeRange &r : scope_ptr->all()) {
                if (r.start_off > off) break;
                if (off >= r.end_off) continue;
                if (tf.scope_needed() && !tf.scope_matches(r)) continue;
                best = &r; // sorted by start: a later start is deeper
            }
            return best;
        };

        // Shared tail of site construction: span guards, content expansion,
        // verb positioning, noop detection. `pat_index` < 0 for anchorless
        // sites ($MATCH is empty there; $ENCLOSING_* still resolves).
        auto add_site = [&](uint64_t ss, uint64_t se, const std::string &pat_id,
                            uint64_t m_from, uint64_t m_to, int pat_index,
                            size_t open_len, size_t close_len) {
            Site st;
            st.span_start = ss;
            st.span_end = se;
            st.pat = pat_id;

            // Span guards run against the resolved span for every verb —
            // a runaway block endangers an insert position just as much.
            if (ed.max_span_lines > 0) {
                uint32_t lo = pf.idx.line_of(ss);
                uint32_t hi = pf.idx.line_of(se > ss ? se - 1 : ss);
                if (hi - lo + 1 > (uint64_t)ed.max_span_lines) {
                    Violation v;
                    v.guard = "max-span-lines";
                    v.file = pf.path;
                    v.line = lo;
                    v.message = "span covers " +
                                std::to_string(hi - lo + 1) +
                                " lines (limit " +
                                std::to_string(ed.max_span_lines) +
                                "); raise -max-span-lines if intended";
                    violations.push_back(std::move(v));
                    return;
                }
            }
            if (!ed.assert_contains.empty()) {
                const char *b = pf.content.data() + ss;
                if (!std::regex_search(b, b + (se - ss), assert_re)) {
                    Violation v;
                    v.guard = "assert-contains";
                    v.file = pf.path;
                    v.line = pf.idx.line_of(ss);
                    v.message = "target span does not match -assert-contains "
                                "'" + ed.assert_contains + "'";
                    violations.push_back(std::move(v));
                    return;
                }
            }

            // Expand content for this site.
            std::string text;
            if (ed.verb != EditOptions::Verb::Delete) {
                if (body_is_template) {
                    TokenCtx tc;
                    tc.match_text = std::string_view(pf.content)
                                        .substr(m_from, m_to - m_from);
                    tc.file = &pf.path;
                    tc.line = pf.idx.line_of(m_from);
                    tc.pat_id = &st.pat;
                    std::vector<std::string> values;
                    if (pat_index >= 0 &&
                        extract_table.has((uint32_t)pat_index)) {
                        extract_table.extract((uint32_t)pat_index,
                                              tc.match_text, values);
                        tc.extract_names =
                            &extract_table.names((uint32_t)pat_index);
                        tc.extract_values = &values;
                    }
                    tc.enclosing =
                        scope_ptr ? scope_ptr->find_innermost(m_from) : nullptr;
                    text = expand_template(body, tc);
                } else {
                    text = body;
                }
            }

            // Position the splice.
            switch (ed.verb) {
                case EditOptions::Verb::Replace:
                case EditOptions::Verb::Delete:
                    st.start = ss;
                    st.end = se;
                    break;
                case EditOptions::Verb::Insert:
                    switch (ed.insert_pos) {
                        case EditOptions::InsertPos::Before:
                            st.start = st.end = ss;
                            break;
                        case EditOptions::InsertPos::After:
                            st.start = st.end = se;
                            break;
                        case EditOptions::InsertPos::Start:
                            st.start = st.end = ss + open_len;
                            break;
                        case EditOptions::InsertPos::End:
                            st.start = st.end = se - close_len;
                            break;
                    }
                    break;
            }
            st.text = std::move(text);
            st.noop =
                (st.end == st.start && st.text.empty()) ||
                (st.end > st.start &&
                 std::string_view(pf.content).substr(st.start,
                                                     st.end - st.start) ==
                     st.text);
            st.line_start = pf.idx.line_of(st.start);
            st.line_end =
                pf.idx.line_of(st.end > st.start ? st.end - 1 : st.start);
            pf.sites.push_back(std::move(st));
        };

        if (!anchorless) {
            for (const Match &m : kept) {
                uint64_t ss = 0, se = 0;
                size_t olen = 0, clen = 0;
                switch (ed.span) {
                    case EditOptions::Span::Match:
                        ss = m.from;
                        se = m.to;
                        break;
                    case EditOptions::Span::Line: {
                        uint64_t ls, le;
                        pf.idx.line_range(m.from, ls, le);
                        ss = ls;
                        uint64_t last = m.to > m.from ? m.to - 1 : m.from;
                        pf.idx.line_range(last, ls, le);
                        se = le < pf.content.size() && pf.content[le] == '\n'
                                 ? le + 1
                                 : le;
                        break;
                    }
                    case EditOptions::Span::Block:
                    case EditOptions::Span::BlockFull: {
                        uint64_t bo = 0, bc = 0;
                        if (!find_balanced_block(pf.content, m.from,
                                                 cli.block_open,
                                                 cli.block_close, bo, bc)) {
                            Violation v;
                            v.guard = "block-not-found";
                            v.file = pf.path;
                            v.line = pf.idx.line_of(m.from);
                            v.message = "no balanced " + cli.block_open +
                                        cli.block_close +
                                        " block found after match";
                            violations.push_back(std::move(v));
                            continue;
                        }
                        ss = ed.span == EditOptions::Span::Block ? bo : m.from;
                        se = bc;
                        olen = cli.block_open.size();
                        clen = cli.block_close.size();
                        break;
                    }
                    case EditOptions::Span::Scope:
                    case EditOptions::Span::ScopeBody: {
                        const ScopeRange *sr = innermost_span_scope(m.from);
                        if (!sr) {
                            Violation v;
                            v.guard = "scope-not-found";
                            v.file = pf.path;
                            v.line = pf.idx.line_of(m.from);
                            v.message = "match is not inside any recognized "
                                        "scope (check -scope pack / "
                                        "-in-scope filter)";
                            violations.push_back(std::move(v));
                            continue;
                        }
                        if (ed.span == EditOptions::Span::Scope) {
                            ss = sr->start_off;
                            se = sr->end_off;
                        } else {
                            uint64_t bo = 0, bc = 0;
                            if (!find_balanced_block(pf.content, sr->start_off,
                                                     scope_cfg.open,
                                                     scope_cfg.close, bo,
                                                     bc)) {
                                Violation v;
                                v.guard = "block-not-found";
                                v.file = pf.path;
                                v.line = sr->line_start;
                                v.message = "no balanced body block for "
                                            "scope '" + sr->name + "'";
                                violations.push_back(std::move(v));
                                continue;
                            }
                            ss = bo;
                            se = bc;
                            olen = scope_cfg.open.size();
                            clen = scope_cfg.close.size();
                        }
                        break;
                    }
                }
                add_site(ss, se, patterns[m.pattern_index].id, m.from, m.to,
                         (int)m.pattern_index, olen, clen);
            }
        } else {
            // Anchorless: one site per scope matching the -in-scope filter.
            // Nested scopes that both match become overlapping sites — the
            // overlap guard turns that into an explicit conflict.
            for (const ScopeRange &r : scope_ptr->all()) {
                if (!tf.scope_matches(r)) continue;
                uint64_t ss = 0, se = 0;
                size_t olen = 0, clen = 0;
                if (ed.span == EditOptions::Span::Scope) {
                    ss = r.start_off;
                    se = r.end_off;
                } else {
                    uint64_t bo = 0, bc = 0;
                    if (!find_balanced_block(pf.content, r.start_off,
                                             scope_cfg.open, scope_cfg.close,
                                             bo, bc)) {
                        Violation v;
                        v.guard = "block-not-found";
                        v.file = pf.path;
                        v.line = r.line_start;
                        v.message =
                            "no balanced body block for scope '" + r.name +
                            "'";
                        violations.push_back(std::move(v));
                        continue;
                    }
                    ss = bo;
                    se = bc;
                    olen = scope_cfg.open.size();
                    clen = scope_cfg.close.size();
                }
                add_site(ss, se, r.name, ss, ss, -1, olen, clen);
            }
        }

        if (!pf.sites.empty()) files.push_back(std::move(pf));
        return !stop_walk;
    });

    // Rebind each LineIndex to its file's final buffer: PlanFile was moved
    // into `files`, and a small (SSO) content string relocates on move,
    // leaving the index's internal view dangling. Offsets and line numbers
    // computed during planning are plain values and stay correct.
    for (PlanFile &pf : files) pf.idx.build(pf.content);

    // ---- dedup + overlap conflicts (per file, byte order) ----
    uint64_t site_count = 0, changed = 0, noops = 0, files_changed = 0;
    for (PlanFile &pf : files) {
        std::sort(pf.sites.begin(), pf.sites.end(),
                  [](const Site &a, const Site &b) {
                      if (a.start != b.start) return a.start < b.start;
                      return a.end < b.end;
                  });
        std::vector<Site> keep;
        keep.reserve(pf.sites.size());
        for (Site &st : pf.sites) {
            if (!keep.empty()) {
                Site &prev = keep.back();
                if (prev.start == st.start && prev.end == st.end) {
                    if (prev.text == st.text) continue; // identical: dedup
                    Violation v;
                    v.guard = "overlap";
                    v.file = pf.path;
                    v.line = st.line_start;
                    v.message = "patterns '" + prev.pat + "' and '" + st.pat +
                                "' produce different content for the same "
                                "byte range";
                    violations.push_back(std::move(v));
                    continue;
                }
                if (st.start < prev.end) {
                    Violation v;
                    v.guard = "overlap";
                    v.file = pf.path;
                    v.line = st.line_start;
                    v.message =
                        "edit for pattern '" + st.pat + "' (line " +
                        std::to_string(st.line_start) +
                        ") overlaps the edit for pattern '" + prev.pat +
                        "' (line " + std::to_string(prev.line_start) + ")";
                    violations.push_back(std::move(v));
                    continue;
                }
            }
            keep.push_back(std::move(st));
        }
        pf.sites = std::move(keep);
        bool any_change = false;
        for (const Site &st : pf.sites) {
            ++site_count;
            if (st.noop) {
                ++noops;
            } else {
                ++changed;
                any_change = true;
            }
        }
        if (any_change) ++files_changed;
    }

    // ---- -expect ----
    if (ed.expect >= 0 && (int64_t)site_count != ed.expect) {
        Violation v;
        v.guard = "expect";
        v.message = "expected exactly " + std::to_string(ed.expect) +
                    " edit site(s), found " + std::to_string(site_count);
        violations.push_back(std::move(v));
    }

    const bool json_records =
        cli.out_mode_set && cli.out_mode == OutputMode::JsonLines;

    auto emit_scan_summary = [&]() {
        if (!cli.summary) return;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t_start)
                           .count();
        emit_summary_record(stats, site_count, (uint64_t)elapsed);
    };

    if (!violations.empty()) {
        for (const Violation &v : violations) emit_guard_record(v);
        emit_scan_summary();
        emit_edit_summary(site_count, changed, noops, files_changed,
                          !ed.write, false);
        return 3;
    }

    if (site_count == 0) {
        emit_scan_summary();
        emit_edit_summary(0, 0, 0, 0, !ed.write, false);
        return 1;
    }

    // ---- dry-run ----
    if (!ed.write) {
        if (!json_records || ed.diff) {
            for (const PlanFile &pf : files) print_file_diff(pf, stdout);
        }
        if (json_records) {
            for (const PlanFile &pf : files)
                for (const Site &st : pf.sites) emit_edit_record(pf, st, cli);
        }
        emit_scan_summary();
        emit_edit_summary(site_count, changed, noops, files_changed, true,
                          false);
        if (cli.require_complete &&
            (stats.files_failed > 0 || stats.missing_paths > 0))
            return 2;
        return 0;
    }

    // ---- apply: drift-check everything, then write everything ----
    for (const PlanFile &pf : files) {
        struct stat stbuf;
        bool drifted = ::stat(pf.write_path.c_str(), &stbuf) != 0 ||
                       stbuf.st_size != pf.st_size ||
                       stat_mtime(stbuf).tv_sec != pf.st_mtim.tv_sec ||
                       stat_mtime(stbuf).tv_nsec != pf.st_mtim.tv_nsec;
        if (drifted) {
            Violation v;
            v.guard = "changed-during-run";
            v.file = pf.path;
            v.message = "file changed between planning and writing; "
                        "re-run to plan against the current content";
            violations.push_back(std::move(v));
        }
    }
    if (!violations.empty()) {
        for (const Violation &v : violations) emit_guard_record(v);
        emit_scan_summary();
        emit_edit_summary(site_count, changed, noops, files_changed, false,
                          false);
        return 3;
    }

    for (const PlanFile &pf : files) {
        bool any_change = false;
        for (const Site &st : pf.sites)
            if (!st.noop) { any_change = true; break; }
        if (!any_change) continue;
        std::string data = build_new_content(pf);
        std::string werr;
        if (!write_file_atomic(pf, data, werr)) {
            std::fprintf(stderr, "hprscript: edit: %s\n", werr.c_str());
            io_error = true;
            break;
        }
    }

    if (ed.diff && !io_error) {
        for (const PlanFile &pf : files) print_file_diff(pf, stdout);
    }
    for (const PlanFile &pf : files)
        for (const Site &st : pf.sites) emit_edit_record(pf, st, cli);
    emit_scan_summary();
    emit_edit_summary(site_count, changed, noops, files_changed, false,
                      !io_error);
    if (io_error) return 2;
    if (cli.require_complete &&
        (stats.files_failed > 0 || stats.missing_paths > 0))
        return 2;
    return 0;
}

} // namespace hpr
