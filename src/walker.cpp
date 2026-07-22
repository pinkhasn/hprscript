#include "walker.hpp"

#include "glob.hpp"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace hpr {

namespace {

// Normalize a relative path to forward-slashes (filesystem on Linux already
// uses '/', so this is mostly a string-conversion safety net).
std::string to_forward(const fs::path &p) {
    std::string s = p.generic_string();
    return s;
}

// True if any segment of `path` equals `name` exactly.
bool path_has_segment(std::string_view path, std::string_view name) {
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (path.substr(start, i - start) == name) return true;
            start = i + 1;
        }
    }
    return false;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

bool Walker::is_excluded(std::string_view path, bool is_dir) const {
    for (const auto &rule : exclude_) {
        if (rule.empty()) continue;
        if (rule.back() == '/') {
            // path-prefix rule, e.g. "vendor/" or "src/generated/"
            std::string_view p(rule);
            p.remove_suffix(1);
            if (path == p || starts_with(path, std::string(p) + "/")) return true;
            continue;
        }
        if (has_glob_chars(rule)) {
            if (glob_match(rule, path)) return true;
            // Also check basename for simple patterns like "*.log".
            auto slash = path.rfind('/');
            std::string_view base =
                slash == std::string_view::npos ? path : path.substr(slash + 1);
            if (glob_match(rule, base)) return true;
            continue;
        }
        // Bare name: match any path segment with this exact name.
        if (path_has_segment(path, rule)) return true;
        if (is_dir && path == rule) return true;
    }
    return false;
}

void Walker::walk(const std::function<bool(const WalkItem &)> &visit) {
    std::unordered_set<std::string> seen;
    bool stop = false;

    auto offer = [&](const std::string &path) -> bool {
        if (stop) return false;
        if (!seen.insert(path).second) return true;
        WalkItem it;
        it.path = path;
        if (!visit(it)) { stop = true; return false; }
        return true;
    };

    auto walk_dir = [&](const std::string &base, const std::string &suffix) {
        std::error_code ec;
        if (!fs::exists(base, ec)) return;
        if (fs::is_regular_file(base, ec)) {
            // Literal file path passed as scan item.
            if (!is_excluded(base, false)) offer(base);
            return;
        }
        // Deterministic DFS: each directory's entries are sorted by name
        // before visiting, so output order is lexicographic pre-order and
        // identical across runs, machines, and filesystems (raw readdir
        // order is arbitrary). Recursion depth = directory depth.
        std::function<void(const std::string &)> walk_one =
            [&](const std::string &dir) {
                std::error_code dec;
                fs::directory_iterator it(
                    dir, fs::directory_options::skip_permission_denied, dec);
                if (dec) return;
                std::vector<fs::directory_entry> entries;
                fs::directory_iterator dend;
                while (it != dend) {
                    entries.push_back(*it);
                    it.increment(dec);
                    if (dec) break;
                }
                std::sort(entries.begin(), entries.end(),
                          [](const fs::directory_entry &a,
                             const fs::directory_entry &b) {
                              return a.path().filename() < b.path().filename();
                          });
                for (const auto &entry : entries) {
                    if (stop) return;
                    std::string raw = to_forward(entry.path());
                    // Strip leading "./" if base is "."; otherwise as-is.
                    std::string disp = raw;
                    if (disp.size() >= 2 && disp[0] == '.' && disp[1] == '/')
                        disp.erase(0, 2);

                    std::string fname = entry.path().filename().string();
                    std::error_code sec;
                    bool is_dir = entry.is_directory(sec);

                    // Hidden files/dirs (leading '.') are always skipped
                    // during recursive descent (explicitly named hidden
                    // bases were handled before we got here).
                    if (!fname.empty() && fname[0] == '.' && fname != "." &&
                        fname != "..")
                        continue;
                    if (is_excluded(disp, is_dir)) continue;

                    if (is_dir) {
                        walk_one(raw);
                        continue;
                    }
                    if (!entry.is_regular_file(sec)) continue;
                    bool match = suffix.empty();
                    if (!match) {
                        // Path relative to base for matching against suffix.
                        std::string rel;
                        if (base == ".") {
                            rel = disp;
                        } else if (starts_with(disp, base + "/")) {
                            rel = disp.substr(base.size() + 1);
                        } else if (disp == base) {
                            rel = entry.path().filename().string();
                        } else {
                            rel = disp;
                        }
                        match = glob_match(suffix, rel);
                    }
                    if (match) {
                        if (!offer(disp)) return;
                    }
                }
            };
        walk_one(base);
    };

    // Literal paths (file lists) first: taken verbatim, never run through
    // split_glob, so glob metacharacters in the names stay literal.
    for (const auto &item : literal_) {
        if (stop) break;
        std::error_code ec;
        if (fs::is_regular_file(item, ec)) {
            if (!is_excluded(item, false)) offer(item);
        } else if (fs::is_directory(item, ec)) {
            walk_dir(item, "");
        }
    }

    for (const auto &item : scan_) {
        if (stop) break;
        std::error_code ec;
        if (fs::exists(item, ec) && !has_glob_chars(item)) {
            if (fs::is_regular_file(item, ec)) {
                if (!is_excluded(item, false)) offer(item);
            } else if (fs::is_directory(item, ec)) {
                walk_dir(item, ""); // no glob suffix → take all files in tree
            }
            continue;
        }
        GlobSplit gs = split_glob(item);
        walk_dir(gs.base, gs.suffix);
    }
}

} // namespace hpr
