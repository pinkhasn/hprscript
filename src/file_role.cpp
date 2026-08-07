#include "file_role.hpp"

#include <algorithm>
#include <cctype>

namespace hpr {

namespace {
bool has(const std::string &s, const std::string &needle) {
    return s.find(needle) != std::string::npos;
}
bool ends(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
void add(std::vector<std::string> &v, const std::string &role) {
    if (std::find(v.begin(), v.end(), role) == v.end()) v.push_back(role);
}
} // namespace

FileRoleResult classify_file_roles(const std::string &path) {
    std::string p = path;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    FileRoleResult out;
    if (has(p, "/test/") || has(p, "/tests/") || has(p, "/spec/") ||
        ends(p, "_test.go") || has(p, ".test.") || has(p, ".spec."))
        add(out.roles, "test");
    if (ends(p, ".json") || ends(p, ".yaml") || ends(p, ".yml") ||
        ends(p, ".toml") || ends(p, ".ini") || ends(p, ".env") ||
        has(p, "/config/") || has(p, "/configs/"))
        add(out.roles, "config");
    if (ends(p, ".md") || ends(p, ".rst") || has(p, "/docs/") ||
        has(p, "/doc/"))
        add(out.roles, "documentation");
    if (ends(p, "makefile") || ends(p, "cmakelists.txt") ||
        ends(p, ".mk") || has(p, "/build/") || has(p, "/.github/workflows/"))
        add(out.roles, "build");
    if (has(p, "/generated/") || has(p, "/vendor/") ||
        has(p, "/node_modules/") || ends(p, ".generated.go"))
        add(out.roles, "generated");
    if (has(p, "/fixture/") || has(p, "/fixtures/") ||
        has(p, "/testdata/"))
        add(out.roles, "fixture");
    if (out.roles.empty() ||
        (out.roles.size() == 1 && out.roles.front() == "test"))
        add(out.roles, "source");
    return out;
}

} // namespace hpr
