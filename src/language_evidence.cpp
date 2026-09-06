#include "language_evidence.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace hpr {

const char *occurrence_role(const RoleIndex *roles, const ScopeIndex *scope,
                           uint64_t from, uint64_t to, uint32_t line,
                           const ScopeRange **definition) {
    if (definition) *definition = nullptr;
    if (roles) {
        if (roles->at(from) == LexRole::Comment) return "comment";
        if (roles->at(from) == LexRole::Str) return "string";
    }
    if (scope) {
        if (const auto *s = scope->declared_at(from, to)) {
            if (definition) *definition = s;
            return "def";
        }
    }
    if (roles && roles->import_line(line)) return "import";
    return nullptr;
}

OccurrenceClassification classify_occurrence(
    const std::string &path, std::string_view content, uint64_t from,
    uint64_t to, const LineIndex &lines, const ScopeIndex *scope,
    const RoleIndex *roles) {
    OccurrenceClassification out;
    const std::string lang = auto_lang_for_path(path);
    out.method = roles && !lang.empty() ? "lexical-" + lang + "-pack"
                                      : "lexical-generic-pack";
    if (const char *role = occurrence_role(roles, scope, from, to, lines.line_of(from))) {
        const std::string r(role);
        out.classification = r == "def" ? "probable_definition"
                           : r == "import" ? "probable_import" : r;
        out.confidence = r == "comment" || r == "string" ? "high" : "medium";
        return out;
    }
    out.classification = "probable_call_or_reference";
    if (from >= to || to > content.size()) return out;
    auto ident = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    if ((from && ident(content[from - 1])) || (to < content.size() && ident(content[to])))
        return out;
    for (uint64_t i = from; i < to; ++i) if (!ident(content[i])) return out;

    // Only inspect a bounded declaration prefix. A type mention somewhere
    // on another function's signature cannot satisfy the declared-name test.
    uint64_t begin = from;
    while (begin && from - begin < 512) {
        char c = content[begin - 1];
        if (c == ';' || c == '{' || c == '}' || c == '\n') break;
        --begin;
    }
    std::string prefix(content.substr(begin, from - begin));
    const bool c_family = lang == "c" || lang == "cpp" || lang == "java";
    if (c_family && prefix.find_first_not_of(" \t\r\n") == std::string::npos) {
        uint32_t line = lines.line_of(from);
        for (unsigned count = 0; line > 1 && count < 8 && prefix.size() < 512; ++count) {
            auto prev = lines.line_text(--line);
            const size_t start = prev.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos || prev.find_first_of(";{}") != std::string_view::npos ||
                (roles && roles->at(prev.data() - content.data() + start) != LexRole::Code)) break;
            prefix = std::string(prev) + '\n' + prefix;
        }
    }
    size_t next = to;
    while (next < content.size() && std::isspace(static_cast<unsigned char>(content[next]))) ++next;
    static const std::regex declared(
        R"(^\s*(?:(?:export|public|private|protected|static|typedef)\s+)*(?:class|struct|enum|type|interface|function|func|fn)\s+$)");
    if (std::regex_match(prefix, declared)) {
        if (c_family && next < content.size() && content[next] != '{' && content[next] != ':' && content[next] != ';')
            return out;
        out.classification = c_family && next < content.size() && content[next] == ';'
                           ? "probable_declaration" : "probable_definition";
        out.confidence = "low";
        return out;
    }
    if (next < content.size() && content[next] == '(' &&
        c_family) {
        // Ordinary prototypes only. Calls such as return f(), x = f(),
        // obj.f(), and bare f() are references; exotic syntax stays uncertain.
        static const std::regex type_prefix(R"(^\s*(?:[A-Za-z_]\w*(?:::\w+)*(?:<[^;{}()=]*>)?[\s*&]+)+$)");
        static const std::regex expression_word(R"(\b(return|throw|co_return|if|while|for|case|new|delete)\b)");
        size_t end = next;
        int depth = 0;
        do {
            if (!roles || roles->at(end) == LexRole::Code) {
                if (content[end] == '(') ++depth;
                else if (content[end] == ')') --depth;
            }
            ++end;
        } while (end < content.size() && end - next < 2048 && depth);
        while (end < content.size() && std::isspace(static_cast<unsigned char>(content[end]))) ++end;
        if (!depth && end < content.size() && content[end] == ';' &&
            std::regex_match(prefix, type_prefix) && !std::regex_search(prefix, expression_word)) {
            out.classification = "probable_declaration";
            out.confidence = "medium";
            return out;
        }
    }
    if (next < content.size() &&
        ((content[next] == '=' && (next + 1 == content.size() || content[next + 1] != '=')) ||
         content.substr(next, 2) == ":=" || (lang.empty() && content[next] == ':'))) {
        out.classification = "probable_assignment";
    }
    return out;
}

} // namespace hpr
