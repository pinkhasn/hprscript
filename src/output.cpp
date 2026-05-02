#include "output.hpp"

#include "block.hpp"

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
    if (!find_balanced_block(buf, m.to, opts.block_open, opts.block_close,
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

void Formatter::emit_llm(const std::string &file, const Pattern &pattern,
                         const Match &m, std::string_view buf,
                         const LineIndex &idx, const ScopeIndex *scope) {
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
    if (opts_.mode == OutputMode::Llm) {
        char buf[160];
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
