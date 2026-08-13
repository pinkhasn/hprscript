#include "expand.hpp"

#include "cli.hpp"
#include "file_io.hpp"
#include "line_index.hpp"
#include "pipeline.hpp" // looks_binary
#include "scope.hpp"
#include "seen.hpp" // fnv1a

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace hpr {

std::string ref_hash6(std::string_view line_text) {
    size_t b = 0, e = line_text.size();
    auto ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (b < e && ws(line_text[b])) ++b;
    while (e > b && ws(line_text[e - 1])) --e;
    uint64_t h = fnv1a(line_text.substr(b, e - b));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06llx",
                  static_cast<unsigned long long>(h & 0xFFFFFFull));
    return buf;
}

namespace {

struct Ref {
    std::string path;
    uint32_t line = 0;
    std::string hash; // empty = unverified
};

// `file:line[@hash]`, parsed from the right so paths containing ':' work.
bool parse_ref(const std::string &s, Ref &out, std::string &err) {
    std::string body = s;
    size_t at = s.rfind('@');
    if (at != std::string::npos) {
        std::string h = s.substr(at + 1);
        bool hex = !h.empty() && h.size() <= 16;
        for (char &c : h) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
        }
        if (!hex) {
            err = "bad ref '" + s + "': '@' must be followed by a hex hash";
            return false;
        }
        out.hash = h;
        body = s.substr(0, at);
    }
    size_t colon = body.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 >= body.size()) {
        err = "bad ref '" + s + "': expected <file>:<line>[@hash]";
        return false;
    }
    uint64_t line = 0;
    for (size_t i = colon + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c < '0' || c > '9') {
            err = "bad ref '" + s + "': line must be a number";
            return false;
        }
        line = line * 10 + static_cast<uint64_t>(c - '0');
        if (line > 0xFFFFFFFFull) {
            err = "bad ref '" + s + "': line out of range";
            return false;
        }
    }
    if (line == 0) {
        err = "bad ref '" + s + "': lines are 1-based";
        return false;
    }
    out.path = body.substr(0, colon);
    out.line = static_cast<uint32_t>(line);
    return true;
}

} // namespace

int run_expand(const Cli &cli) {
    if (cli.positional.empty()) {
        std::fprintf(stderr,
                     "hprscript: expand: at least one <file:line[@hash]> ref "
                     "required\n");
        return 2;
    }
    if (!cli.scope_lang.empty() && cli.scope_lang != "auto" &&
        !builtin_scope_pack(cli.scope_lang)) {
        std::fprintf(stderr,
                     "hprscript: unknown -scope pack '%s' (supported: auto, "
                     "go, rust, c, cpp, java, js, ts)\n",
                     cli.scope_lang.c_str());
        return 2;
    }

    ScopeConfig custom;
    custom.anchor_regex = cli.scope_pattern;
    custom.open = cli.scope_open;
    custom.close = cli.scope_close;
    custom.kind = cli.scope_kind;
    const std::string eff_lang =
        cli.scope_lang.empty() && cli.scope_pattern.empty() ? "auto"
                                                            : cli.scope_lang;

    int exit_code = 0;
    bool first_out = true;

    // Print lines [lo, hi], stopping at -max-block-bytes with an explicit
    // truncation marker rather than a mid-line cut.
    auto print_lines = [&](const LineIndex &idx, uint32_t lo, uint32_t hi,
                           bool numbered, uint32_t mark) {
        uint64_t used = 0;
        for (uint32_t L = lo; L <= hi; ++L) {
            std::string_view t = idx.line_text(L);
            if (cli.max_block_bytes > 0 &&
                used + t.size() > cli.max_block_bytes) {
                std::printf("\xE2\x80\xA6 (+%u more lines, -max-block-bytes "
                            "reached)\n",
                            hi - L + 1);
                return;
            }
            used += t.size();
            if (numbered)
                std::printf("%s%u: ", L == mark ? "> " : "  ", L);
            std::fwrite(t.data(), 1, t.size(), stdout);
            if (t.empty() || t.back() != '\n') std::printf("\n");
        }
    };

    for (const auto &raw : cli.positional) {
        Ref ref;
        std::string perr;
        if (!parse_ref(raw, ref, perr)) {
            std::fprintf(stderr, "hprscript: expand: %s\n", perr.c_str());
            return 2;
        }
        MappedFile mf;
        if (!mf.open(ref.path)) {
            std::fprintf(stderr, "hprscript: cannot read %s: %s\n",
                         ref.path.c_str(), std::strerror(errno));
            exit_code = std::max(exit_code, 2);
            continue;
        }
        std::string_view buf = mf.view();
        if (looks_binary(buf)) {
            std::fprintf(stderr, "hprscript: expand: %s is binary\n",
                         ref.path.c_str());
            exit_code = std::max(exit_code, 2);
            continue;
        }
        LineIndex idx;
        idx.build(buf);

        if (!first_out) std::printf("\n");
        first_out = false;

        // Verify / recover the ref's line. Stale refs are reported in-band —
        // for the reader, "the thing you saw is gone" is a result, not a
        // diagnostic.
        uint32_t line = ref.line;
        std::string moved_note;
        const bool line_exists = line <= idx.line_count();
        if (!ref.hash.empty()) {
            if (!line_exists || ref_hash6(idx.line_text(line)) != ref.hash) {
                uint32_t found = 0;
                int count = 0;
                for (uint32_t L = 1; L <= idx.line_count() && count < 2; ++L)
                    if (ref_hash6(idx.line_text(L)) == ref.hash) {
                        found = L;
                        ++count;
                    }
                if (count == 1) {
                    moved_note = " (ref line moved: " +
                                 std::to_string(ref.line) + " \xE2\x86\x92 " +
                                 std::to_string(found) + ")";
                    line = found;
                } else {
                    std::printf("%s: stale \xE2\x80\x94 line %u changed and "
                                "%s\n",
                                raw.c_str(), ref.line,
                                count == 0
                                    ? "no line with this hash exists"
                                    : "multiple lines share this hash");
                    exit_code = std::max(exit_code, 3);
                    continue;
                }
            }
        } else if (!line_exists) {
            std::printf("%s: stale \xE2\x80\x94 file has only %u lines\n",
                        raw.c_str(), idx.line_count());
            exit_code = std::max(exit_code, 3);
            continue;
        }

        ScopeIndex scope;
        bool have_scope = false;
        ScopeConfig sc = resolve_scope_for_file(eff_lang, custom, ref.path);
        if (!sc.anchor_regex.empty()) {
            std::string serr;
            if (scope.build(buf, sc, idx, &serr)) have_scope = true;
        }
        const ScopeRange *sr = nullptr;
        if (have_scope) {
            sr = scope.anchor_on_line(line);
            if (!sr) {
                std::string_view t = idx.line_text(line);
                if (t.data() != nullptr)
                    sr = scope.find_innermost(
                        static_cast<uint64_t>(t.data() - buf.data()));
            }
        }

        if (sr) {
            std::printf("%s:%u-%u %s %s%s\n", ref.path.c_str(),
                        sr->line_start, sr->line_end, sr->kind.c_str(),
                        sr->name.c_str(), moved_note.c_str());
            print_lines(idx, sr->line_start, sr->line_end,
                        /*numbered=*/false, 0);
        } else {
            // No enclosing scope: a numbered context window with the ref
            // line marked, since there's no signature to anchor the eye.
            uint32_t before = static_cast<uint32_t>(cli.context_before);
            uint32_t after = static_cast<uint32_t>(cli.context_after);
            if (before == 0 && after == 0) before = after = 5;
            uint32_t lo = line > before ? line - before : 1;
            uint32_t hi = std::min(line + after, idx.line_count());
            std::printf("%s:%u (no enclosing scope)%s\n", ref.path.c_str(),
                        line, moved_note.c_str());
            print_lines(idx, lo, hi, /*numbered=*/true, line);
        }
    }
    return exit_code;
}

} // namespace hpr
