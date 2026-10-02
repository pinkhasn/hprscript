// Small string helpers shared across hprscript components.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace hpr {

// Largest input handed to std::regex. libstdc++'s matcher recurses per input
// character and overflows an 8 MB stack somewhere past ~20 KB, so text taken
// from scanned files must be capped before it reaches std::regex.
constexpr size_t kMaxStdRegexInput = 4096;

inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// C-style identifier: [A-Za-z_][A-Za-z0-9_]*.
inline bool is_identifier(std::string_view s) {
    if (s.empty() || (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_'))
        return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

// Backslash-escape regex metacharacters so `s` matches literally.
inline std::string regex_escape(std::string_view s) {
    static const char *special = "\\^$.[]|()?*+{}";
    std::string out;
    for (char c : s) {
        if (std::strchr(special, c)) out += '\\';
        out += c;
    }
    return out;
}

// Heuristic: a NUL byte in the first 512 bytes means binary content.
inline bool looks_binary(std::string_view content) {
    size_t n = std::min<size_t>(content.size(), 512);
    for (size_t i = 0; i < n; ++i) {
        if (content[i] == '\0') return true;
    }
    return false;
}

} // namespace hpr
