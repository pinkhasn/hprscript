// Recursive file walker for scan/exclude rules.
//
// Aggregates scan items (literal files, directories, and globs) plus exclude
// rules, then yields candidate file paths to a visitor. Exclude rules accept
// three forms (per SRSCRIPT.md):
//   1. glob pattern         "*.log", "src/**/*.test.go"
//   2. bare directory name  "vendor"   — match against any path segment
//   3. path prefix with /   "vendor/", "src/generated/"
//
// Hidden directories (starting with '.') are skipped automatically. Files
// whose first 512 bytes contain a NUL byte are reported as binary and not
// scanned (callers can check this via the `is_binary` flag).
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hpr {

struct WalkItem {
    std::string path;       // path as it should appear in output
    bool is_binary = false; // detected NUL within first 512 bytes
};

class Walker {
public:
    // scan = literal paths (file or dir) or globs (with *, **, ?, [...]).
    void add_scan(const std::string &item) { scan_.push_back(item); }
    void add_exclude(const std::string &rule) { exclude_.push_back(rule); }

    // Walk all scan items; visit each candidate file (after exclude rules,
    // hidden-dir skip, and binary check). Visitor returns false to stop.
    void walk(const std::function<bool(const WalkItem &)> &visit);

private:
    bool is_excluded(std::string_view path, bool is_dir) const;

    std::vector<std::string> scan_;
    std::vector<std::string> exclude_;
};

// Helper used by Walker and test helpers — peek first 512 bytes for NUL.
bool file_looks_binary(const std::string &path);

} // namespace hpr
