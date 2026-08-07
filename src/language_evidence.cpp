#include "language_evidence.hpp"

#include "scope.hpp"

#include <algorithm>
#include <cctype>

namespace hpr {

namespace {
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}
bool contains(const std::string &s, const char *v) {
    return s.find(v) != std::string::npos;
}
} // namespace

OccurrenceClassification classify_occurrence(const std::string &path,
                                               std::string_view line,
                                               std::string_view matched,
                                               const std::string &profile,
                                               bool test_file) {
    OccurrenceClassification out;
    const std::string lang = auto_lang_for_path(path);
    out.method = lang.empty() ? "lexical-generic-pack" : "lexical-" + lang + "-pack";
    const std::string l = lower(line);
    const std::string m = lower(matched);
    if (test_file) {
        out.classification = "probable_test_reference";
        out.confidence = "medium";
        return out;
    }
    if (contains(l, "import ") || contains(l, "#include") ||
        contains(l, "require(") || contains(l, " from ")) {
        out.classification = "probable_import";
        out.confidence = "medium";
    } else if (contains(l, "func ") || contains(l, "function ") ||
               contains(l, "class ") || contains(l, "struct ") ||
               contains(l, "enum ") || contains(l, "type ")) {
        out.classification = "probable_definition";
        out.confidence = "medium";
    } else if (contains(l, "extern ") || contains(l, "interface ") ||
               (!m.empty() && contains(l, (m + ";").c_str()))) {
        out.classification = "probable_declaration";
        out.confidence = "low";
    } else if (contains(l, "=") || contains(l, ":=")) {
        out.classification = "probable_assignment";
        out.confidence = "low";
    } else {
        out.classification = "probable_call_or_reference";
        out.confidence = "low";
    }
    if (profile == "error" &&
        (contains(l, "throw") || contains(l, "return") || contains(l, "wrap") ||
         contains(l, "log") || contains(l, "assert")))
        out.confidence = "medium";
    return out;
}

} // namespace hpr
