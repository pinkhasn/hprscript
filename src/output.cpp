#include "output.hpp"

#include "block.hpp"
#include "seen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/types.h>

namespace hpr {

namespace {

// Resolved block info for one match. Filled in by extract_block when the
// formatter has both delimiters set; otherwise `found` stays false and the
// `$BLOCK*` tokens / fields are omitted.
struct BlockInfo {
    bool active = false;  // both open and close were configured
    bool found = false;   // a balanced block was located
    uint64_t start = 0;   // open delim byte offset
    uint64_t end = 0;     // close delim byte offset (exclusive)
    uint32_t line_start = 0;
    uint32_t line_end = 0;
    std::string_view block_text;       // [start, end)
    std::string_view block_full_text;  // [match.from, end)
};

BlockInfo extract_block(const OutputOptions &opts, std::string_view buf,
                        const Match &m, const LineIndex &idx,
                        bool need_lines) {
    BlockInfo bi;
    if (opts.block_open.empty() || opts.block_close.empty()) return bi;
    bi.active = true;
    uint64_t op = 0, cp = 0;
    // Search from match START: an opener contained in the match (a signature
    // regex ending in `\{`, a PEM `-----BEGIN` header) anchors its own block.
    if (!find_balanced_block(buf, m.from, opts.block_open, opts.block_close,
                             op, cp))
        return bi;
    bi.found = true;
    bi.start = op;
    bi.end = cp;
    bi.block_text = std::string_view(buf.data() + op, cp - op);
    bi.block_full_text = std::string_view(buf.data() + m.from, cp - m.from);
    if (need_lines) {
        bi.line_start = idx.line_of(op);
        bi.line_end = idx.line_of(cp == 0 ? 0 : cp - 1);
    }
    return bi;
}

} // namespace

void json_escape_to(std::string &out, std::string_view s) {
    out.reserve(out.size() + s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

std::string json_escape(std::string_view s) {
    std::string out;
    json_escape_to(out, s);
    return out;
}

void emit_warning_record(const char *code, const std::string &path) {
    std::string s = "{\"type\":\"warning\",\"code\":\"";
    s += code;
    s += "\",\"file\":\"";
    json_escape_to(s, path);
    s += "\"}\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
}

void emit_summary_record(const ScanStats &st, uint64_t emitted,
                         uint64_t elapsed_ms) {
    std::string s = "{\"type\":\"summary\",\"files_scanned\":";
    s += std::to_string(st.files_scanned);
    s += ",\"bytes_scanned\":";
    s += std::to_string(st.bytes_scanned);
    s += ",\"files_skipped_binary\":";
    s += std::to_string(st.files_binary);
    s += ",\"files_failed\":";
    s += std::to_string(st.files_failed);
    s += ",\"missing_paths\":";
    s += std::to_string(st.missing_paths);
    s += ",\"matches\":";
    s += std::to_string(st.matches_seen);
    s += ",\"matches_seen\":";
    s += std::to_string(st.matches_seen);
    s += ",\"scan_stages\":";
    s += std::to_string(st.scan_stages);
    s += ",\"matcher_compilations\":";
    s += std::to_string(st.matcher_compilations);
    s += ",\"patterns_compiled\":";
    s += std::to_string(st.patterns_compiled);
    s += ",\"rows_materialized\":";
    s += std::to_string(st.rows_materialized);
    s += ",\"rows_output\":";
    s += std::to_string(st.rows_output ? st.rows_output : emitted);
    s += ",\"rows_truncated\":";
    s += std::to_string(st.rows_truncated);
    s += ",\"buffered_bytes_peak\":";
    s += std::to_string(st.buffered_bytes_peak);
    s += ",\"emitted\":";
    s += std::to_string(emitted);
    s += ",\"complete\":";
    s += st.complete() ? "true" : "false";
    if (!st.stop_reason.empty()) {
        s += ",\"stop_reason\":\"";
        s += st.stop_reason;
        s += '"';
    }
    s += ",\"elapsed_ms\":";
    s += std::to_string(elapsed_ms);
    s += "}\n";
    std::fwrite(s.data(), 1, s.size(), stdout);
}

namespace {

void append_uint(std::string &out, uint64_t v) {
    char buf[24];
    int n = std::snprintf(buf, sizeof(buf), "%llu",
                          static_cast<unsigned long long>(v));
    if (n > 0) out.append(buf, static_cast<size_t>(n));
}

void append_uint32(std::string &out, uint32_t v) {
    append_uint(out, static_cast<uint64_t>(v));
}

// Truncate `sv` to at most `limit` bytes, backing off to the last full UTF-8
// codepoint boundary so we never emit a half-codepoint. limit=0 means no cap.
// Returns the (possibly shorter) view; sets *truncated when the result is
// shorter than the input.
std::string_view truncate_safe(std::string_view sv, uint64_t limit,
                               bool *truncated) {
    if (truncated) *truncated = false;
    if (limit == 0 || sv.size() <= limit) return sv;
    size_t n = static_cast<size_t>(limit);
    // Walk back to the start of a codepoint: leading bytes are 0xxxxxxx or
    // 11xxxxxx; continuation bytes are 10xxxxxx. If we landed on a
    // continuation byte, step back until we find a leading byte (or hit 0).
    while (n > 0 && (static_cast<unsigned char>(sv[n]) & 0xC0) == 0x80) --n;
    if (truncated) *truncated = true;
    return sv.substr(0, n);
}

} // namespace

Formatter::Formatter(OutputOptions opts, FILE *out) : opts_(opts), out_(out) {}

void Formatter::write_out(const char *data, size_t n) {
    std::fwrite(data, 1, n, out_);
    bytes_emitted_ += n;
    if (opts_.max_output_bytes > 0 && bytes_emitted_ >= opts_.max_output_bytes)
        over_budget_ = true;
}

void Formatter::refresh_file_cache(const std::string &file) {
    if (cached_file_ == file) return;
    cached_file_ = file;
    cached_file_esc_.clear();
    json_escape_to(cached_file_esc_, file);
}

void Formatter::refresh_pattern_cache(const Pattern &pattern) {
    if (cached_pat_ == &pattern) return;
    cached_pat_ = &pattern;
    cached_pat_id_esc_.clear();
    json_escape_to(cached_pat_id_esc_, pattern.id);
}

void Formatter::on_record_absent(const std::string &file,
                                 const Pattern &pattern, uint32_t line,
                                 std::string_view text) {
    if (opts_.mode != OutputMode::Absent) return;
    refresh_file_cache(file);
    refresh_pattern_cache(pattern);
    bool rec_trunc = false;
    std::string_view rec =
        truncate_safe(text, opts_.max_context_bytes, &rec_trunc);
    auto &s = scratch_;
    s.clear();
    s += "{\"file\":\"";
    s += cached_file_esc_;
    s += "\",\"pat\":\"";
    s += cached_pat_id_esc_;
    s += "\",\"line\":";
    append_uint32(s, line);
    s += ",\"record\":\"";
    json_escape_to(s, rec);
    s += '"';
    if (rec_trunc) s += ",\"record_truncated\":true,\"truncated\":true";
    s += "}\n";
    write_out(s.data(), s.size());
    ++emitted_;
}

std::string_view Formatter::context_block(std::string_view buf,
                                          const LineIndex &idx,
                                          const Match &m) const {
    uint32_t line = idx.line_of(m.from);
    uint32_t first = (line > static_cast<uint32_t>(opts_.context_before))
                         ? line - static_cast<uint32_t>(opts_.context_before)
                         : 1;
    uint32_t last = std::min<uint32_t>(line + opts_.context_after, idx.line_count());
    auto first_sv = idx.line_text(first);
    auto last_sv = idx.line_text(last);
    if (first_sv.data() == nullptr || last_sv.data() == nullptr) return {};
    const char *start = first_sv.data();
    const char *end = last_sv.data() + last_sv.size();
    return std::string_view(start, static_cast<size_t>(end - start));
}

void Formatter::emit_json(const std::string &file, const Pattern &pattern,
                          const Match &m, std::string_view buf,
                          const LineIndex &idx, const ScopeIndex *scope) {
    refresh_file_cache(file);
    refresh_pattern_cache(pattern);

    uint32_t line = idx.line_of(m.from);
    uint32_t col = idx.col_of(m.from);
    std::string_view match_text(buf.data() + m.from, m.to - m.from);
    std::string_view ctx = context_block(buf, idx, m);
    BlockInfo bi = extract_block(opts_, buf, m, idx, /*need_lines=*/true);

    bool match_trunc = false, ctx_trunc = false;
    bool block_trunc = false, block_full_trunc = false;
    std::string_view match_view  = truncate_safe(match_text, opts_.max_match_bytes, &match_trunc);
    std::string_view ctx_view    = truncate_safe(ctx,       opts_.max_context_bytes, &ctx_trunc);
    std::string_view block_view, block_full_view;
    if (bi.found) {
        block_view      = truncate_safe(bi.block_text,      opts_.max_block_bytes, &block_trunc);
        block_full_view = truncate_safe(bi.block_full_text, opts_.max_block_bytes, &block_full_trunc);
    }
    bool any_trunc = match_trunc || ctx_trunc || block_trunc || block_full_trunc;

    auto &s = scratch_;
    s.clear();
    s += "{\"file\":\"";
    s += cached_file_esc_;
    s += '"';
    if (!pattern.id.empty()) {
        s += ",\"pat\":\"";
        s += cached_pat_id_esc_;
        s += '"';
    }
    s += ",\"line\":";
    append_uint32(s, line);
    s += ",\"col\":";
    append_uint32(s, col);
    s += ",\"from\":";
    append_uint(s, m.from);
    s += ",\"to\":";
    append_uint(s, m.to);
    s += ",\"match\":\"";
    json_escape_to(s, match_view);
    s += "\",\"context\":\"";
    json_escape_to(s, ctx_view);
    s += "\"";
    if (bi.found) {
        s += ",\"block\":\"";
        json_escape_to(s, block_view);
        s += "\",\"block_full\":\"";
        json_escape_to(s, block_full_view);
        s += "\",\"block_start\":";
        append_uint(s, bi.start);
        s += ",\"block_end\":";
        append_uint(s, bi.end);
        s += ",\"block_line_start\":";
        append_uint32(s, bi.line_start);
        s += ",\"block_line_end\":";
        append_uint32(s, bi.line_end);
    }
    if (match_trunc)       s += ",\"match_truncated\":true";
    if (ctx_trunc)         s += ",\"context_truncated\":true";
    if (block_trunc)       s += ",\"block_truncated\":true";
    if (block_full_trunc)  s += ",\"block_full_truncated\":true";
    if (any_trunc)         s += ",\"truncated\":true";
    if (opts_.extract_table && opts_.extract_table->has(m.pattern_index)) {
        std::vector<std::string> values;
        opts_.extract_table->extract(m.pattern_index, match_text, values);
        const auto &names = opts_.extract_table->names(m.pattern_index);
        s += ",\"extracted\":{";
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) s += ',';
            s += '"';
            json_escape_to(s, names[i]);
            s += "\":\"";
            json_escape_to(s, values[i]);
            s += '"';
        }
        s += '}';
    }
    if (scope) {
        if (const ScopeRange *sr = scope->find_innermost(m.from)) {
            s += ",\"enclosing\":{\"name\":\"";
            json_escape_to(s, sr->name);
            s += "\",\"kind\":\"";
            json_escape_to(s, sr->kind);
            s += "\",\"line_start\":";
            append_uint32(s, sr->line_start);
            s += ",\"line_end\":";
            append_uint32(s, sr->line_end);
            s += "}";
        }
    }
    s += "}\n";
    write_out(s);
}

void Formatter::emit_match_only(std::string_view buf, const Match &m,
                                const LineIndex &idx) {
    BlockInfo bi = extract_block(opts_, buf, m, idx, /*need_lines=*/false);
    std::string_view text = bi.found
        ? bi.block_full_text
        : std::string_view(buf.data() + m.from, m.to - m.from);
    uint64_t limit = bi.found ? opts_.max_block_bytes : opts_.max_match_bytes;
    text = truncate_safe(text, limit, nullptr);
    auto &s = scratch_;
    s.clear();
    s.append(text.data(), text.size());
    s += '\n';
    write_out(s);
}

void Formatter::emit_custom(const std::string &file, const Pattern &pattern,
                            const Match &m, std::string_view buf,
                            const LineIndex &idx, const ScopeIndex *scope) {
    uint32_t line = idx.line_of(m.from);
    uint32_t col = idx.col_of(m.from);
    std::string_view match_text(buf.data() + m.from, m.to - m.from);
    std::string_view ctx = context_block(buf, idx, m);
    BlockInfo bi = extract_block(opts_, buf, m, idx, /*need_lines=*/true);

    std::string_view match_view = truncate_safe(match_text, opts_.max_match_bytes, nullptr);
    std::string_view ctx_view   = truncate_safe(ctx, opts_.max_context_bytes, nullptr);
    std::string_view block_view = truncate_safe(bi.block_text, opts_.max_block_bytes, nullptr);
    std::string_view block_full_view = truncate_safe(bi.block_full_text, opts_.max_block_bytes, nullptr);

    // Lazily-extracted capture groups (only populated if the format template
    // actually references $EXTRACT_*).
    std::vector<std::string> extracted_values;
    bool extracted_done = false;
    const std::vector<std::string> *extract_names = nullptr;
    if (opts_.extract_table && opts_.extract_table->has(m.pattern_index)) {
        extract_names = &opts_.extract_table->names(m.pattern_index);
    }
    auto ensure_extracted = [&]() {
        if (extracted_done || !extract_names) return;
        extracted_done = true;
        opts_.extract_table->extract(m.pattern_index, match_text, extracted_values);
    };

    const std::string &fmt = opts_.format_template;
    auto &out = scratch_;
    out.clear();
    out.reserve(fmt.size() + match_view.size() + ctx_view.size() + 32);
    auto try_str = [&](size_t &i, const char *tok, std::string_view val) -> bool {
        size_t n = std::strlen(tok);
        if (fmt.compare(i, n, tok) != 0) return false;
        out.append(val.data(), val.size());
        i += n;
        return true;
    };
    auto try_num = [&](size_t &i, const char *tok, uint64_t v) -> bool {
        size_t n = std::strlen(tok);
        if (fmt.compare(i, n, tok) != 0) return false;
        append_uint(out, v);
        i += n;
        return true;
    };
    const ScopeRange *sr = (scope ? scope->find_innermost(m.from) : nullptr);
    static const char EXTRACT_PREFIX[] = "$EXTRACT_";
    static constexpr size_t EXTRACT_PREFIX_LEN = sizeof(EXTRACT_PREFIX) - 1;
    for (size_t i = 0; i < fmt.size();) {
        if (fmt[i] != '$') { out += fmt[i++]; continue; }
        // Order matters — longer tokens first so $BLOCK_LINE_START doesn't
        // get short-circuited by $BLOCK.
        if (try_num(i, "$BLOCK_LINE_START", bi.line_start)) continue;
        if (try_num(i, "$BLOCK_LINE_END", bi.line_end)) continue;
        if (try_num(i, "$BLOCK_LINE_COUNT",
                    bi.found ? bi.line_end - bi.line_start + 1 : 0)) continue;
        if (try_num(i, "$BLOCK_BYTE_COUNT", bi.end - bi.start)) continue;
        if (try_num(i, "$BLOCK_START", bi.start)) continue;
        if (try_num(i, "$BLOCK_END", bi.end)) continue;
        if (try_str(i, "$BLOCK_FULL", block_full_view)) continue;
        if (try_str(i, "$BLOCK", block_view)) continue;
        if (try_str(i, "$CONTEXT", ctx_view)) continue;
        if (try_str(i, "$MATCH", match_view)) continue;
        if (try_str(i, "$FILE", file)) continue;
        if (try_num(i, "$LINE", line)) continue;
        if (try_num(i, "$COL", col)) continue;
        if (try_num(i, "$FROM", m.from)) continue;
        if (try_num(i, "$TO", m.to)) continue;
        if (try_str(i, "$PAT_ID", pattern.id)) continue;
        if (try_str(i, "$ENCLOSING_NAME", sr ? std::string_view(sr->name) : std::string_view{})) continue;
        if (try_str(i, "$ENCLOSING_KIND", sr ? std::string_view(sr->kind) : std::string_view{})) continue;
        if (try_num(i, "$ENCLOSING_LINE_START", sr ? sr->line_start : 0)) continue;
        if (try_num(i, "$ENCLOSING_LINE_END",   sr ? sr->line_end   : 0)) continue;
        // $EXTRACT_<NAME> — read identifier suffix and look up in `names`.
        if (extract_names && fmt.compare(i, EXTRACT_PREFIX_LEN, EXTRACT_PREFIX) == 0) {
            size_t e = i + EXTRACT_PREFIX_LEN;
            while (e < fmt.size() && (std::isalnum((unsigned char)fmt[e]) || fmt[e] == '_'))
                ++e;
            std::string upper(fmt.data() + i + EXTRACT_PREFIX_LEN,
                              e - (i + EXTRACT_PREFIX_LEN));
            // Match against the user's name (case-sensitive); we uppercase
            // expected when comparing both as-is. To allow $EXTRACT_NAME to
            // match a name like "name", do case-insensitive compare here.
            ssize_t hit = -1;
            for (size_t k = 0; k < extract_names->size(); ++k) {
                const std::string &n = (*extract_names)[k];
                if (n.size() != upper.size()) continue;
                bool eq_i = true;
                for (size_t j = 0; j < n.size(); ++j) {
                    char c1 = std::toupper((unsigned char)n[j]);
                    char c2 = std::toupper((unsigned char)upper[j]);
                    if (c1 != c2) { eq_i = false; break; }
                }
                if (eq_i) { hit = static_cast<ssize_t>(k); break; }
            }
            if (hit >= 0) {
                ensure_extracted();
                out.append(extracted_values[hit]);
                i = e;
                continue;
            }
        }
        out += fmt[i++];
    }
    out += '\n';
    write_out(out);
}

void Formatter::emit_header() {
    if (header_done_) return;
    header_done_ = true;
    if (opts_.mode != OutputMode::Llm && opts_.mode != OutputMode::Elide)
        return;
    if (opts_.header_lines.empty()) return;
    auto &s = scratch_;
    s.clear();
    for (const auto &line : opts_.header_lines) {
        s += line;
        s += '\n';
    }
    write_out(s);
}

void Formatter::emit_llm(const std::string &file, const Pattern &pattern,
                         const Match &m, std::string_view buf,
                         const LineIndex &idx, const ScopeIndex *scope) {
    emit_header();
    auto &s = scratch_;

    // Per-file header: print the path once per consecutive run on that file.
    if (llm_last_file_ != file) {
        llm_last_file_ = file;
        s.clear();
        s.append(file);
        s += '\n';
        write_out(s);
    }

    BlockInfo bi = extract_block(opts_, buf, m, idx, /*need_lines=*/true);

    if (bi.found) {
        // Block branch: header + raw block body, no JSON escaping.
        std::string_view body = truncate_safe(bi.block_full_text,
                                              opts_.max_block_bytes, nullptr);
        s.clear();
        s += "  ";
        append_uint32(s, bi.line_start);
        s += '-';
        append_uint32(s, bi.line_end);
        if (opts_.pattern_count > 1 && !pattern.id.empty()) {
            s += " [";
            s += pattern.id;
            s += ']';
        }
        s += '\n';
        s.append(body.data(), body.size());
        if (body.empty() || body.back() != '\n') s += '\n';
        write_out(s);
        return;
    }

    // Line branch: "  LINE: [pat?] context [in scope]?".
    uint32_t line = idx.line_of(m.from);
    std::string_view ctx = context_block(buf, idx, m);
    std::string_view ctx_view = truncate_safe(ctx, opts_.max_context_bytes,
                                              nullptr);

    s.clear();
    s += "  ";
    append_uint32(s, line);
    s += ": ";
    if (opts_.pattern_count > 1 && !pattern.id.empty()) {
        s += '[';
        s += pattern.id;
        s += "] ";
    }
    // Strip a single trailing newline from the context (line text usually
    // ends with one) so we don't break the per-record-on-one-line shape.
    if (!ctx_view.empty() && ctx_view.back() == '\n')
        ctx_view.remove_suffix(1);
    s.append(ctx_view.data(), ctx_view.size());
    if (scope) {
        if (const ScopeRange *sr = scope->find_innermost(m.from)) {
            s += "  [in ";
            s.append(sr->kind);
            s += ' ';
            s.append(sr->name);
            s += ']';
        }
    }
    s += '\n';
    write_out(s);
}

namespace {

// Small gaps inside a scope are shown plainly rather than elided — "…
// (+1 lines)" saves nothing and reads worse than just printing the line.
constexpr uint32_t ELIDE_MERGE_GAP = 2;

struct LineWindow {
    uint32_t lo, hi;
};

} // namespace

void Formatter::on_file_elide(const std::string &file,
                              const std::vector<Match> &kept,
                              std::string_view buf, const LineIndex &idx,
                              const ScopeIndex *scope, const SeenStore *seen,
                              std::vector<SeenMark> *marks_out) {
    if (kept.empty()) return;
    emit_header();
    emitted_ += static_cast<uint64_t>(kept.size());

    bool first_block = true;
    auto &s = scratch_;

    auto print_file_header = [&]() {
        if (llm_last_file_ != file) {
            llm_last_file_ = file;
            s.clear();
            s.append(file);
            s += '\n';
            write_out(s);
        }
    };

    auto print_line_range = [&](uint32_t lo, uint32_t hi) {
        for (uint32_t L = lo; L <= hi; ++L) {
            std::string_view t = idx.line_text(L);
            s.clear();
            s.append(t.data(), t.size());
            if (t.empty() || t.back() != '\n') s += '\n';
            write_out(s);
        }
    };

    // Lines strictly between from_excl and to_excl: shown plainly if few,
    // elided as "… (+N lines)" otherwise.
    auto print_gap = [&](uint32_t from_excl, uint32_t to_excl) {
        if (to_excl <= from_excl + 1) return;
        uint32_t n = to_excl - from_excl - 1;
        if (n <= ELIDE_MERGE_GAP) {
            print_line_range(from_excl + 1, to_excl - 1);
        } else {
            s.clear();
            s += "  \xE2\x80\xA6 (+";
            append_uint32(s, n);
            s += " lines)\n";
            write_out(s);
        }
    };

    auto render_scope_block = [&](const ScopeRange &sr,
                                  const std::vector<const Match *> &ms) {
        uint64_t hash = 0;
        bool collapse = false;
        if (seen || marks_out) {
            hash = fnv1a(std::string_view(
                buf.data() + sr.start_off, sr.end_off - sr.start_off));
            if (marks_out)
                marks_out->push_back({file, sr.line_start, sr.line_end, hash});
            if (seen && seen->seen_unchanged(file, sr.line_start, sr.line_end, hash))
                collapse = true;
        }

        print_file_header();
        s.clear();
        if (!first_block) s += '\n';
        first_block = false;
        s += "  ";
        append_uint32(s, sr.line_start);
        s += '-';
        append_uint32(s, sr.line_end);
        s += ' ';
        s += sr.kind;
        s += ' ';
        s += sr.name;
        if (collapse) s += " (unchanged, already shown)";
        s += '\n';
        write_out(s);
        if (collapse) return;
        print_line_range(sr.line_start, sr.line_start); // signature line

        std::vector<LineWindow> windows;
        windows.reserve(ms.size());
        for (const Match *m : ms) {
            uint32_t line = idx.line_of(m->from);
            uint32_t lo = (line > static_cast<uint32_t>(opts_.context_before))
                ? line - static_cast<uint32_t>(opts_.context_before)
                : 1;
            uint32_t hi = line + static_cast<uint32_t>(opts_.context_after);
            lo = std::max(lo, sr.line_start + 1);
            hi = std::min({hi, sr.line_end, idx.line_count()});
            if (lo > hi) continue; // whole window swallowed by the signature line
            windows.push_back({lo, hi});
        }
        // Merge overlapping/near windows (already position-sorted, since
        // `ms` follows the position-sorted `kept` order).
        std::vector<LineWindow> merged;
        for (const auto &w : windows) {
            if (!merged.empty() &&
                w.lo <= merged.back().hi + ELIDE_MERGE_GAP + 1) {
                merged.back().hi = std::max(merged.back().hi, w.hi);
            } else {
                merged.push_back(w);
            }
        }

        uint32_t prev_end = sr.line_start;
        for (const auto &w : merged) {
            print_gap(prev_end, w.lo);
            print_line_range(w.lo, w.hi);
            prev_end = w.hi;
        }
        if (prev_end < sr.line_end) {
            print_gap(prev_end, sr.line_end);
            print_line_range(sr.line_end, sr.line_end); // closing line
        }
    };

    auto render_orphan = [&](const Match *m) {
        print_file_header();
        if (!first_block) {
            s.clear();
            s += '\n';
            write_out(s);
        }
        first_block = false;
        uint32_t line = idx.line_of(m->from);
        std::string_view ctx = context_block(buf, idx, *m);
        std::string_view ctx_view =
            truncate_safe(ctx, opts_.max_context_bytes, nullptr);
        s.clear();
        s += "  ";
        append_uint32(s, line);
        s += ": ";
        s.append(ctx_view.data(), ctx_view.size());
        if (s.empty() || s.back() != '\n') s += '\n';
        write_out(s);
    };

    // Walk in position order, flushing an accumulated run whenever the
    // innermost enclosing scope changes. `kept` is sorted by `from`, so
    // same-scope matches are contiguous except for nesting, which — being
    // rare in practice — isn't special-cased further.
    const ScopeRange *cur_scope = nullptr;
    bool run_is_scope = false;
    std::vector<const Match *> run;

    auto flush = [&]() {
        if (run.empty()) return;
        if (run_is_scope) {
            render_scope_block(*cur_scope, run);
        } else {
            for (const Match *m : run) render_orphan(m);
        }
        run.clear();
    };

    for (const auto &m : kept) {
        const ScopeRange *sr = scope ? scope->find_innermost(m.from) : nullptr;
        bool is_scope = sr != nullptr;
        if (!run.empty() && (is_scope != run_is_scope || sr != cur_scope))
            flush();
        if (run.empty()) {
            run_is_scope = is_scope;
            cur_scope = sr;
        }
        run.push_back(&m);
    }
    flush();
}

void Formatter::on_match(const std::string &file, const Pattern &pattern,
                         const Match &m, std::string_view buf,
                         const LineIndex &idx, const ScopeIndex *scope) {
    ++emitted_;
    switch (opts_.mode) {
        case OutputMode::JsonLines:   emit_json(file, pattern, m, buf, idx, scope); break;
        case OutputMode::FilesOnly:
            // Print the file once, on first match. Counter tracked below.
            if (per_file_counts_[file]++ == 0) {
                write_out(file);
                write_out("\n", 1);
            }
            break;
        case OutputMode::Counts:
            per_file_counts_[file]++;
            break;
        case OutputMode::MatchOnly:   emit_match_only(buf, m, idx); break;
        case OutputMode::Custom:      emit_custom(file, pattern, m, buf, idx, scope); break;
        case OutputMode::Llm:         emit_llm(file, pattern, m, buf, idx, scope); break;
        case OutputMode::Elide:
            // Elide renders a whole file's matches at once via
            // on_file_elide(); it never streams through on_match(). Undo
            // the ++emitted_ above so callers can't double-count by mistake.
            --emitted_;
            break;
        case OutputMode::Absent:
            // Absent mode tracks per-file presence, no per-match output.
            per_file_counts_[file]++;
            break;
    }
}

void Formatter::on_file_end(const std::string &file, bool had_match) {
    switch (opts_.mode) {
        case OutputMode::Counts: {
            auto it = per_file_counts_.find(file);
            uint64_t n = it == per_file_counts_.end() ? 0 : it->second;
            char buf[32];
            int k = std::snprintf(buf, sizeof(buf), ":%llu\n",
                                  static_cast<unsigned long long>(n));
            write_out(file);
            if (k > 0) write_out(buf, static_cast<size_t>(k));
            break;
        }
        case OutputMode::Absent:
            if (!had_match) {
                write_out(file);
                write_out("\n", 1);
            }
            break;
        default: break;
    }
}

void Formatter::on_complete() {
    if (opts_.mode == OutputMode::Llm || opts_.mode == OutputMode::Elide) {
        char buf[160];
        if (!zero_match_ids_.empty()) {
            // A batched pattern that matched nothing must be stated, not
            // inferred from its absence in the output. When the scan stopped
            // early (limit / output budget) the claim is only valid for the
            // scanned prefix, so it's qualified.
            emit_header();
            std::string s = (limit_hit_ || over_budget_)
                ? "--- no matches (scan stopped early): "
                : "--- no matches: ";
            for (size_t i = 0; i < zero_match_ids_.size(); ++i) {
                if (i) s += ", ";
                s += zero_match_ids_[i];
            }
            s += " (";
            s += std::to_string(zero_match_ids_.size());
            s += " of ";
            s += std::to_string(patterns_total_);
            s += patterns_total_ == 1 ? " pattern) ---\n" : " patterns) ---\n";
            std::fwrite(s.data(), 1, s.size(), out_);
        }
        if (limit_hit_) {
            int n = std::snprintf(buf, sizeof(buf),
                "--- limit reached: stopped at %llu matches; more may exist (re-run with -limit 0 for all) ---\n",
                static_cast<unsigned long long>(emitted_));
            if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), out_);
        }
        if (over_budget_) {
            int n = std::snprintf(buf, sizeof(buf),
                "--- output-byte budget reached after %llu matches; output truncated ---\n",
                static_cast<unsigned long long>(emitted_));
            if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), out_);
        }
    } else if (over_budget_) {
        char buf[96];
        int n = std::snprintf(buf, sizeof(buf),
                              "{\"info\":\"output_truncated\",\"emitted\":%llu}\n",
                              static_cast<unsigned long long>(emitted_));
        if (n > 0) std::fwrite(buf, 1, static_cast<size_t>(n), out_);
    }
    std::fflush(out_);
}

} // namespace hpr
