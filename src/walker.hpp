// Recursive file walker for scan/exclude rules.
//
// Aggregates scan items (literal files, directories, and globs) plus exclude
// rules, then yields candidate file paths to a visitor. Exclude rules accept
// three forms:
//   1. glob pattern         "*.log", "src/**/*.test.go"
//   2. bare directory name  "vendor"   — match against any path segment
//   3. path prefix with /   "vendor/", "src/generated/"
//
// Hidden directories (starting with '.') are skipped automatically. The
// walker yields candidate file paths after filtering; callers can cheaply
// inspect the mapped contents to decide whether to scan them.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct WalkItem {
    std::string path;       // path as it should appear in output
};

class Walker {
public:
    // scan = literal paths (file or dir) or globs (with *, **, ?, [...]).
    void add_scan(const std::string &item) { scan_.push_back(item); }
    // literal = a path taken verbatim (from -files-from / -files0-from):
    // never glob-interpreted, so names containing *, {, [ stay literal.
    // Regular files are offered (exclude rules still apply); directories are
    // walked recursively; anything else is ignored — callers validate and
    // warn about missing entries before adding.
    void add_literal(const std::string &path) { literal_.push_back(path); }
    void add_exclude(const std::string &rule) { exclude_.push_back(rule); }

    // Walk all scan items; visit each candidate file after exclude rules and
    // hidden-dir skipping. Visitor returns false to stop.
    // Optional read-only diagnostics for callers that promise complete
    // traversal accounting. Existing streaming callers retain their behavior.
    void walk(const std::function<bool(const WalkItem &)> &visit,
              const std::function<void(const std::string &)> &error = {});

private:
    bool is_excluded(std::string_view path, bool is_dir) const;

    std::vector<std::string> scan_;
    std::vector<std::string> literal_;
    std::vector<std::string> exclude_;
};

} // namespace hpr
