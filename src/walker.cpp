#include "walker.hpp"

#include "glob.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace hpr {

bool file_looks_binary(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char buf[512];
    in.read(buf, sizeof(buf));
    auto n = in.gcount();
    for (std::streamsize i = 0; i < n; ++i) {
        if (buf[i] == '\0') return true;
    }
    return false;
}

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
        it.is_binary = file_looks_binary(path);
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
        // Recursive walk; we manage the stack so we can skip dirs efficiently.
        fs::recursive_directory_iterator it(
            base, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        while (it != end && !stop) {
            const auto &entry = *it;
            std::string raw = to_forward(entry.path());
            // Strip leading "./" if base is "."; otherwise display as-is.
            std::string disp = raw;
            if (disp.size() >= 2 && disp[0] == '.' && disp[1] == '/')
                disp.erase(0, 2);

            std::string fname = entry.path().filename().string();
            bool is_dir = entry.is_directory(ec);

            // Hidden files/dirs (leading '.') are always skipped, except the
            // base dir itself which is not "." here (we already strip "./").
            if (!fname.empty() && fname[0] == '.' && fname != "." && fname != "..") {
                if (is_dir) it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }

            if (is_excluded(disp, is_dir)) {
                if (is_dir) it.disable_recursion_pending();
                it.increment(ec);
                continue;
            }

            if (!is_dir && entry.is_regular_file(ec)) {
                bool match = suffix.empty();
                if (!match) {
                    // Compute path relative to base for matching against suffix.
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
            it.increment(ec);
        }
    };

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
