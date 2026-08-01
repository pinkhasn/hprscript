#include "ident.hpp"

#include <cctype>

namespace hpr {

std::vector<size_t> identifier_subtoken_starts(std::string_view s) {
    std::vector<size_t> starts;
    const size_t n = s.size();
    auto is_lower = [](char c) { return c >= 'a' && c <= 'z'; };
    auto is_upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '_') { ++i; continue; }

        if (is_digit(c)) {
            starts.push_back(i);
            size_t j = i + 1;
            while (j < n && is_digit(s[j])) ++j;
            i = j;
            continue;
        }
        if (is_upper(c)) {
            starts.push_back(i);
            size_t j = i + 1;
            while (j < n && is_upper(s[j])) ++j;
            if (j < n && is_lower(s[j]) && j > i + 1) {
                // Acronym run followed by a lowercase word: "HTTPServer" —
                // the last uppercase letter starts the next word, so back
                // off one position ("HTTP" | "Server", not "HTTPS" | "erver").
                i = j - 1;
            } else if (j == i + 1 && j < n && is_lower(s[j])) {
                // Single leading capital + lowercase run: "Config".
                size_t k = j;
                while (k < n && is_lower(s[k])) ++k;
                i = k;
            } else {
                i = j; // trailing acronym with nothing lowercase after it
            }
            continue;
        }
        if (is_lower(c)) {
            starts.push_back(i);
            size_t j = i + 1;
            while (j < n && is_lower(s[j])) ++j;
            i = j;
            continue;
        }
        ++i; // unreachable given the caller's [A-Za-z0-9_]+ charset
    }
    return starts;
}

namespace {

bool group_matches(std::string_view ident, const std::vector<size_t> &starts,
                   const std::vector<std::string> &terms_lower) {
    for (const auto &term : terms_lower) {
        bool found = false;
        for (size_t s : starts) {
            if (s + term.size() > ident.size()) continue;
            bool eq = true;
            for (size_t k = 0; k < term.size(); ++k) {
                if (std::tolower(static_cast<unsigned char>(ident[s + k])) !=
                    static_cast<unsigned char>(term[k])) {
                    eq = false;
                    break;
                }
            }
            if (eq) { found = true; break; }
        }
        if (!found) return false; // AND within the group
    }
    return true;
}

} // namespace

void scan_identifiers(std::string_view buf, const std::vector<IdentGroup> &groups,
                      uint32_t pattern_index_base, std::vector<Match> &out) {
    if (groups.empty()) return;
    const size_t n = buf.size();
    auto is_ident_start = [](unsigned char c) {
        return std::isalpha(c) || c == '_';
    };
    auto is_ident_cont = [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    };

    size_t i = 0;
    while (i < n) {
        if (!is_ident_start(static_cast<unsigned char>(buf[i]))) { ++i; continue; }
        size_t start = i++;
        while (i < n && is_ident_cont(static_cast<unsigned char>(buf[i]))) ++i;
        std::string_view ident = buf.substr(start, i - start);
        std::vector<size_t> starts = identifier_subtoken_starts(ident);
        for (size_t g = 0; g < groups.size(); ++g) {
            if (group_matches(ident, starts, groups[g].terms)) {
                Match m;
                m.pattern_index = pattern_index_base + static_cast<uint32_t>(g);
                m.from = start;
                m.to = i;
                out.push_back(m);
            }
        }
    }
}

} // namespace hpr
