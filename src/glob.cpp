#include "glob.hpp"

#include <vector>

namespace hpr {

namespace {

std::vector<std::string_view> split_path(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/') {
            if (i > start) out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// Match a character class starting at pat[pi] (pat[pi] == '['). On success,
// advances pi past the closing ']'. Returns whether `c` is in the class.
bool match_class(std::string_view pat, size_t &pi, char c) {
    ++pi; // skip '['
    bool negate = false;
    if (pi < pat.size() && (pat[pi] == '!' || pat[pi] == '^')) {
        negate = true;
        ++pi;
    }
    bool found = false;
    while (pi < pat.size() && pat[pi] != ']') {
        char a = pat[pi++];
        if (pi + 1 < pat.size() && pat[pi] == '-' && pat[pi + 1] != ']') {
            char b = pat[++pi];
            ++pi;
            if (c >= a && c <= b) found = true;
        } else if (a == c) {
            found = true;
        }
    }
    if (pi < pat.size()) ++pi; // skip ']'
    return found ^ negate;
}

// Match a single path segment (no '/' in either side).
bool match_segment(std::string_view pat, std::string_view txt) {
    size_t pi = 0, ti = 0;
    size_t star_pi = std::string_view::npos, star_ti = 0;
    while (ti < txt.size()) {
        if (pi < pat.size() && pat[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (pi < pat.size() && pat[pi] == '?') {
            ++pi; ++ti;
        } else if (pi < pat.size() && pat[pi] == '[') {
            size_t saved = pi;
            if (match_class(pat, pi, txt[ti])) {
                ++ti;
            } else if (star_pi != std::string_view::npos) {
                pi = star_pi + 1;
                ti = ++star_ti;
            } else {
                pi = saved; // restore for clarity, will return false below
                return false;
            }
        } else if (pi < pat.size() && pat[pi] == txt[ti]) {
            ++pi; ++ti;
        } else if (star_pi != std::string_view::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

// Expand the first {a,b,...} alternation (skipping any '{' inside a [...]
// class) and recurse so nested / later groups expand too. A '{' with no
// matching '}' is kept as a literal. The cap bounds pathological inputs;
// real patterns ("*.{yml,yaml}") expand to a handful of alternatives.
constexpr size_t MAX_BRACE_EXPANSIONS = 256;

void expand_braces_into(std::string_view pat, std::vector<std::string> &out) {
    if (out.size() >= MAX_BRACE_EXPANSIONS) return;
    size_t open = std::string_view::npos;
    bool in_class = false;
    for (size_t i = 0; i < pat.size(); ++i) {
        char c = pat[i];
        if (in_class) { if (c == ']') in_class = false; continue; }
        if (c == '[') { in_class = true; continue; }
        if (c == '{') { open = i; break; }
    }
    if (open == std::string_view::npos) {
        out.emplace_back(pat);
        return;
    }
    int depth = 0;
    size_t close = std::string_view::npos;
    for (size_t i = open; i < pat.size(); ++i) {
        if (pat[i] == '{') ++depth;
        else if (pat[i] == '}') { if (--depth == 0) { close = i; break; } }
    }
    if (close == std::string_view::npos) { // unmatched → literal
        out.emplace_back(pat);
        return;
    }
    std::string_view content = pat.substr(open + 1, close - open - 1);
    size_t start = 0;
    int d = 0;
    for (size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || (content[i] == ',' && d == 0)) {
            std::string next;
            next.reserve(pat.size());
            next.append(pat.substr(0, open));
            next.append(content.substr(start, i - start));
            next.append(pat.substr(close + 1));
            expand_braces_into(next, out);
            if (out.size() >= MAX_BRACE_EXPANSIONS) return;
            start = i + 1;
        } else if (content[i] == '{') {
            ++d;
        } else if (content[i] == '}') {
            --d;
        }
    }
}

bool match_segments(const std::vector<std::string_view> &ps,
                    const std::vector<std::string_view> &ts,
                    size_t i, size_t j) {
    while (i < ps.size() && j < ts.size()) {
        if (ps[i] == "**") {
            // ** at end matches all remaining segments.
            if (i + 1 == ps.size()) return true;
            for (size_t k = j; k <= ts.size(); ++k) {
                if (match_segments(ps, ts, i + 1, k)) return true;
            }
            return false;
        }
        if (!match_segment(ps[i], ts[j])) return false;
        ++i; ++j;
    }
    while (i < ps.size() && ps[i] == "**") ++i;
    return i == ps.size() && j == ts.size();
}

} // namespace

bool has_glob_chars(std::string_view s) {
    for (char c : s) {
        if (c == '*' || c == '?' || c == '[' || c == '{') return true;
    }
    return false;
}

bool glob_match(std::string_view pat, std::string_view path) {
    auto ts = split_path(path);
    if (pat.find('{') != std::string_view::npos) {
        std::vector<std::string> alts;
        expand_braces_into(pat, alts);
        for (const auto &alt : alts) {
            auto ps = split_path(alt);
            if (match_segments(ps, ts, 0, 0)) return true;
        }
        return false;
    }
    auto ps = split_path(pat);
    return match_segments(ps, ts, 0, 0);
}

GlobSplit split_glob(std::string_view pat) {
    GlobSplit out;
    bool absolute = !pat.empty() && pat.front() == '/';
    auto segs = split_path(pat);
    size_t magic = segs.size();
    for (size_t i = 0; i < segs.size(); ++i) {
        if (has_glob_chars(segs[i])) { magic = i; break; }
    }
    if (absolute) out.base = "/";
    for (size_t i = 0; i < magic; ++i) {
        if (!out.base.empty() && out.base.back() != '/') out.base += '/';
        out.base.append(segs[i].data(), segs[i].size());
    }
    for (size_t i = magic; i < segs.size(); ++i) {
        if (!out.suffix.empty()) out.suffix += '/';
        out.suffix.append(segs[i].data(), segs[i].size());
    }
    if (out.base.empty()) out.base = ".";
    return out;
}

} // namespace hpr
