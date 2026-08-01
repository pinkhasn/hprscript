// Git-aware input selection: shell out to `git` for file lists and
// added-line tables, so scans can target exactly what a change touched.
//
// All paths returned are usable from the current working directory: when the
// cwd is not the repository toplevel, git's toplevel-relative names are
// prefixed with the toplevel path.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpr {

struct GitSelection {
    bool changed = false;              // -git-changed: diff vs HEAD
    bool staged = false;               // -git-staged: diff --cached
    bool untracked = false;            // -git-untracked: ls-files -o
    std::vector<std::string> ranges;   // -git-range A...B (repeatable)

    bool any() const {
        return changed || staged || untracked || !ranges.empty();
    }
    // True when at least one diff-based mode is active (added-line tables
    // need a diff; untracked alone has none).
    bool any_diff() const { return changed || staged || !ranges.empty(); }
};

// File names selected by the active modes (union, deduplicated, deleted
// files excluded). Returns false with `err` set when git fails (not a
// repository, bad range, ...).
bool git_select_files(const GitSelection &sel, std::vector<std::string> &out,
                      std::string &err);

// Added-line table built from `git diff -U0` over the diff-based modes.
// Untracked files (when sel.untracked) appear with whole_file = true.
// Line numbers refer to the diff's new side — exact when the working tree
// matches the diff target.
struct AddedLines {
    bool whole_file = false;
    std::vector<uint32_t> lines; // sorted ascending
};
bool git_added_lines(const GitSelection &sel,
                     std::unordered_map<std::string, AddedLines> &out,
                     std::string &err);

// Commit-churn table: number of commits touching each file in the last
// `days` days (`git log --since=<days>.days.ago --name-only`), one `git`
// invocation regardless of how many files exist — never one subprocess per
// file. Files with no commits in the window are simply absent from `out`.
bool git_churn(int days, std::unordered_map<std::string, uint32_t> &out,
               std::string &err);

} // namespace hpr
