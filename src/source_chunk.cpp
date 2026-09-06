#include "source_chunk.hpp"

#include "output.hpp"

#include <algorithm>
#include <limits>

namespace hpr {
namespace {

std::string quote(std::string_view text) {
    std::string out = "\"";
    json_escape_to(out, text);
    return out + '"';
}

std::string_view line_view(const SourceLine &line, int level) {
    std::string_view text = line.text;
    // The signature/anchor fallback must also work for a minified single
    // line; changing only line ranges would never make that row fit.
    if (level == 2 && text.size() > 256) {
        size_t end = 256;
        while (end && (static_cast<unsigned char>(text[end]) & 0xc0u) == 0x80u) --end;
        text = text.substr(0, end);
    }
    return text;
}

bool keep_line(const SourceChunk &s, uint32_t line, int level) {
    if (level == 0) return true;
    if (level >= 3) return false;
    if (s.anchors.count(line)) return true;
    if (line >= s.signature_first && line <= s.signature_last) return true;
    if (level == 1)
        for (uint32_t anchor : s.anchors)
            if (line + 2 >= anchor && line <= static_cast<uint64_t>(anchor) + 2) return true;
    return false;
}

std::vector<std::pair<uint32_t, uint32_t>> omitted_ranges(const SourceChunk &s, int level) {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    uint32_t next = s.first;
    for (const auto &[line, text] : s.lines) {
        if (!keep_line(s, line, level)) continue;
        if (line > next) ranges.emplace_back(next, line - 1);
        next = line + 1;
    }
    if (next <= s.last) ranges.emplace_back(next, s.last);
    return ranges;
}

} // namespace

uint64_t SourceChunk::memory_bytes() const {
    uint64_t bytes = sizeof(*this) + file.size() + (anchors.size() + requested_lines.size()) * 48;
    for (const auto &[line, text] : lines) bytes += 96 + text.text.size();
    return bytes;
}

SourceChunk make_source_chunk(const std::string &file, const LineIndex &idx,
                              const ScopeRange *scope, uint32_t anchor,
                              const Cli &cli, uint64_t retention_limit) {
    SourceChunk s;
    s.file = file;
    s.anchors.insert(anchor);
    const uint32_t before = cli.context_set ? cli.context_before : 2;
    const uint32_t after = cli.context_set ? cli.context_after : 2;
    uint32_t lo = anchor > before ? anchor - before : 1;
    uint32_t hi = static_cast<uint32_t>(std::min<uint64_t>(idx.line_count(), uint64_t(anchor) + after));
    if (scope && !cli.context_set) {
        lo = std::max(lo, scope->signature_line);
        hi = std::min(hi, scope->line_end);
    }
    s.first = scope ? std::min(scope->signature_line, lo) : lo;
    s.last = scope ? std::max(scope->line_end, hi) : hi;
    s.reliable_scope = scope && scope->source_reliable;
    if (scope) {
        s.body_first = scope->signature_line;
        s.body_last = scope->line_end;
        s.signature_first = scope->signature_line;
        s.signature_last = scope->signature_end_line;
    }
    // Retain anchors first, then their explicit context and signature, then
    // a small complete body. Huge windows/signatures cannot allocate past
    // the shared retained-evidence allowance.
    uint64_t used = s.memory_bytes(), source_bytes = 0;
    auto add = [&](uint32_t line) {
        if (line < 1 || line > idx.line_count() || s.lines.count(line)) return true;
        auto raw = idx.line_text(line);
        uint64_t cap = raw.size();
        if (cli.max_context_bytes) cap = std::min(cap, cli.max_context_bytes);
        if (cli.max_block_bytes) cap = std::min(cap, cli.max_block_bytes > source_bytes
                                                        ? cli.max_block_bytes - source_bytes : 0);
        if (retention_limit) {
            if (used + 144 >= retention_limit) { s.retention_reduced = true; return false; }
            if (cap > retention_limit - used - 144) s.retention_reduced = true;
            cap = std::min(cap, retention_limit - used - 144);
        }
        while (cap && cap < raw.size() && (static_cast<unsigned char>(raw[cap]) & 0xc0u) == 0x80u) --cap;
        if (!cap && !raw.empty()) return false;
        SourceLine value{std::string(raw.substr(0, cap)), raw.size() - cap};
        s.lines.emplace(line, std::move(value));
        used += 144 + cap;
        source_bytes += cap;
        return true;
    };
    add(anchor);
    // Requested context is represented by bounds when huge; do not build a
    // separate unbounded set just to account for omitted requested lines.
    s.requested_lines.insert(lo);
    s.requested_lines.insert(hi);
    if (scope)
        for (uint32_t line = s.signature_first; line <= s.signature_last; ++line)
            if (!add(line)) break;
    for (uint32_t line = lo; line <= hi; ++line)
        if (!add(line)) break;
    if (s.reliable_scope && scope->line_end - scope->signature_line + 1 <= investigation_small_body_lines)
        for (uint32_t line = scope->signature_line; line <= scope->line_end; ++line)
            if (!add(line)) break;
    return s;
}

void merge_source_chunk(SourceChunk &into, const SourceChunk &other) {
    into.retention_reduced = into.retention_reduced || other.retention_reduced;
    into.first = std::min(into.first, other.first);
    into.last = std::max(into.last, other.last);
    into.anchors.insert(other.anchors.begin(), other.anchors.end());
    into.requested_lines.insert(other.requested_lines.begin(), other.requested_lines.end());
    for (const auto &[line, value] : other.lines) {
        auto found = into.lines.find(line);
        if (found == into.lines.end() || found->second.text.size() < value.text.size())
            into.lines[line] = value;
    }
}

SourceChunk focus_source_chunk(const SourceChunk &source, const std::set<uint32_t> &anchors,
                               const Cli &cli) {
    SourceChunk out = source;
    out.anchors = anchors;
    out.requested_lines.clear();
    const uint32_t before = cli.context_set ? cli.context_before : 2;
    const uint32_t after = cli.context_set ? cli.context_after : 2;
    const bool small = out.reliable_scope && out.body_last - out.body_first + 1 <= investigation_small_body_lines;
    out.first = out.body_first ? out.body_first : source.last;
    out.last = out.body_last;
    for (uint32_t anchor : anchors) {
        uint32_t lo = anchor > before ? anchor - before : 1;
        uint32_t hi = static_cast<uint32_t>(std::min<uint64_t>(source.last, uint64_t(anchor) + after));
        if (out.body_first && !cli.context_set) {
            lo = std::max(lo, out.body_first);
            hi = std::min(hi, out.body_last);
        }
        out.requested_lines.insert(lo);
        out.requested_lines.insert(hi);
        out.first = std::min(out.first, lo);
        out.last = std::max(out.last, hi);
    }
    for (auto it = out.lines.begin(); it != out.lines.end();) {
        const uint32_t line = it->first;
        bool keep = (small && line >= out.body_first && line <= out.body_last) ||
                    (line >= out.signature_first && line <= out.signature_last);
        for (uint32_t anchor : anchors)
            if (uint64_t(line) + before >= anchor && line <= uint64_t(anchor) + after) keep = true;
        if (!keep || line < out.first || line > out.last) it = out.lines.erase(it);
        else ++it;
    }
    return out;
}

bool source_chunk_complete(const SourceChunk &s, int level) {
    if (!omitted_ranges(s, level).empty()) return false;
    for (const auto &[line, text] : s.lines)
        if (keep_line(s, line, level) && (text.omitted_bytes || line_view(text, level).size() < text.text.size())) return false;
    return true;
}

std::string render_source_chunk(const SourceChunk &s, const std::string &id, int level, bool llm) {
    const auto omitted = omitted_ranges(s, level);
    const bool complete = source_chunk_complete(s, level);
    bool signature_complete = s.reliable_scope;
    for (uint32_t line = s.signature_first; signature_complete && line <= s.signature_last; ++line) {
        auto it = s.lines.find(line);
        if (it == s.lines.end() || !keep_line(s, line, level) || it->second.omitted_bytes ||
            line_view(it->second, level).size() < it->second.text.size())
            signature_complete = false;
    }
    bool context_reduced = false;
    if (!s.requested_lines.empty()) {
        const uint32_t lo = *s.requested_lines.begin(), hi = *s.requested_lines.rbegin();
        for (auto [first, last] : omitted)
            if (first <= hi && last >= lo) context_reduced = true;
        for (const auto &[line, text] : s.lines)
            if (line >= lo && line <= hi && (text.omitted_bytes || line_view(text, level).size() < text.text.size())) context_reduced = true;
    }
    const std::string mode = level == 3 ? "location" : complete && s.reliable_scope ? "full_body"
                                          : s.reliable_scope ? "structural_excerpt" : "lexical_window";
    if (llm) {
        std::string out = "SOURCE " + id + " " + s.file + ":" + std::to_string(s.first) + "-" +
            std::to_string(s.last) + " [" + mode + "; signature_complete=" +
            (signature_complete ? "yes" : "no") + (context_reduced ? "; context_reduced=yes" : "") + "]\n";
        uint32_t next = s.first;
        for (const auto &[line, text] : s.lines) {
            if (!keep_line(s, line, level)) continue;
            if (line > next) out += "  ... omitted lines " + std::to_string(next) + "-" + std::to_string(line - 1) + "\n";
            const auto view = line_view(text, level);
            const uint64_t omitted_bytes = text.omitted_bytes + text.text.size() - view.size();
            out += "  " + std::to_string(line) + ": " + std::string(view) + "\n";
            if (omitted_bytes) out += "  [line " + std::to_string(line) + ": omitted " + std::to_string(omitted_bytes) + " trailing bytes]\n";
            next = line + 1;
        }
        if (next <= s.last) out += "  ... omitted lines " + std::to_string(next) + "-" + std::to_string(s.last) + "\n";
        return out;
    }
    std::string out = "{\"type\":\"investigation-source\",\"source_chunk_id\":" + quote(id) +
        ",\"file\":" + quote(s.file) + ",\"line_start\":" + std::to_string(s.first) +
        ",\"line_end\":" + std::to_string(s.last) + ",\"mode\":" + quote(mode) +
        ",\"body_complete\":" + (complete && s.reliable_scope ? "true" : "false") +
        ",\"signature_complete\":" + (signature_complete ? "true" : "false") +
        ",\"context_reduced\":" + (context_reduced ? "true" : "false") + ",\"lines\":[";
    bool first = true;
    for (const auto &[line, text] : s.lines) {
        if (!keep_line(s, line, level)) continue;
        if (!first) out += ',';
        first = false;
        const auto view = line_view(text, level);
        const uint64_t omitted_bytes = text.omitted_bytes + text.text.size() - view.size();
        out += "{\"line\":" + std::to_string(line) + ",\"text\":" + quote(view);
        if (omitted_bytes) out += ",\"omitted_bytes\":" + std::to_string(omitted_bytes);
        out += '}';
    }
    out += "],\"omitted_ranges\":[";
    first = true;
    for (auto [lo, hi] : omitted) {
        if (!first) out += ',';
        first = false;
        out += '[' + std::to_string(lo) + ',' + std::to_string(hi) + ']';
    }
    return out + "]}\n";
}

} // namespace hpr
