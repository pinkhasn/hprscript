#include "git.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace hpr {

namespace {

// Run `git <args>`, capturing stdout and stderr separately (both drained via
// poll so neither pipe can fill up and deadlock). Returns false when git
// can't be spawned or exits non-zero; `err` carries git's stderr (or the
// spawn error).
bool run_git(const std::vector<std::string> &args, std::string &out,
             std::string &err) {
    out.clear();
    int outp[2], errp[2];
    if (::pipe(outp) != 0) {
        err = std::strerror(errno);
        return false;
    }
    if (::pipe(errp) != 0) {
        err = std::strerror(errno);
        ::close(outp[0]); ::close(outp[1]);
        return false;
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        err = std::strerror(errno);
        ::close(outp[0]); ::close(outp[1]);
        ::close(errp[0]); ::close(errp[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(outp[1], 1);
        ::dup2(errp[1], 2);
        ::close(outp[0]); ::close(outp[1]);
        ::close(errp[0]); ::close(errp[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>("git"));
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp("git", argv.data());
        // exec failed — report via the (redirected) stderr and bail.
        const char *msg = "hprscript: cannot execute git\n";
        ssize_t ignored = ::write(2, msg, std::strlen(msg));
        (void)ignored;
        ::_exit(127);
    }
    ::close(outp[1]);
    ::close(errp[1]);

    std::string err_buf;
    struct pollfd fds[2] = {{outp[0], POLLIN, 0}, {errp[0], POLLIN, 0}};
    int open_fds = 2;
    char buf[64 * 1024];
    while (open_fds > 0) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP)))
                continue;
            ssize_t n = ::read(fds[i].fd, buf, sizeof(buf));
            if (n > 0) {
                (i == 0 ? out : err_buf).append(buf, (size_t)n);
            } else if (n == 0 || (n < 0 && errno != EINTR)) {
                ::close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
            }
        }
    }

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        // Trim a trailing newline for cleaner one-line error messages.
        while (!err_buf.empty() && (err_buf.back() == '\n' || err_buf.back() == '\r'))
            err_buf.pop_back();
        err = err_buf.empty() ? "git exited with an error" : err_buf;
        return false;
    }
    return true;
}

// Toplevel-prefix for git's repo-relative paths: empty when the cwd IS the
// toplevel, otherwise "<toplevel>/" so paths resolve from the cwd.
bool repo_prefix(std::string &prefix, std::string &err) {
    std::string out;
    if (!run_git({"rev-parse", "--show-toplevel"}, out, err)) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    std::error_code ec;
    fs::path top = fs::weakly_canonical(out, ec);
    fs::path cwd = fs::weakly_canonical(fs::current_path(ec), ec);
    prefix = (top == cwd) ? "" : top.generic_string() + "/";
    return true;
}

void split_nul(const std::string &blob, const std::string &prefix,
               std::vector<std::string> &out) {
    size_t start = 0;
    for (size_t i = 0; i <= blob.size(); ++i) {
        if (i == blob.size() || blob[i] == '\0') {
            if (i > start) out.push_back(prefix + blob.substr(start, i - start));
            start = i + 1;
        }
    }
}

// The `git diff` argument tails for each diff-based mode.
std::vector<std::vector<std::string>> diff_mode_args(const GitSelection &sel) {
    std::vector<std::vector<std::string>> modes;
    if (sel.changed) modes.push_back({"HEAD"});
    if (sel.staged) modes.push_back({"--cached"});
    for (const auto &r : sel.ranges) modes.push_back({r});
    return modes;
}

} // namespace

bool git_select_files(const GitSelection &sel, std::vector<std::string> &out,
                      std::string &err) {
    std::string prefix;
    if (!repo_prefix(prefix, err)) return false;

    std::vector<std::string> collected;
    for (const auto &mode : diff_mode_args(sel)) {
        std::vector<std::string> args = {"-c", "core.quotePath=false", "diff",
                                         "--name-only", "-z",
                                         "--diff-filter=d"};
        args.insert(args.end(), mode.begin(), mode.end());
        std::string blob;
        if (!run_git(args, blob, err)) return false;
        split_nul(blob, prefix, collected);
    }
    if (sel.untracked) {
        std::string blob;
        if (!run_git({"-c", "core.quotePath=false", "ls-files", "--others",
                      "--exclude-standard", "-z"},
                     blob, err))
            return false;
        split_nul(blob, prefix, collected);
    }

    // Union across modes: dedupe, preserving first-seen order.
    std::vector<std::string> seen;
    for (auto &p : collected) {
        if (std::find(seen.begin(), seen.end(), p) == seen.end()) {
            seen.push_back(p);
            out.push_back(std::move(p));
        }
    }
    return true;
}

bool git_added_lines(const GitSelection &sel,
                     std::unordered_map<std::string, AddedLines> &out,
                     std::string &err) {
    std::string prefix;
    if (!repo_prefix(prefix, err)) return false;

    for (const auto &mode : diff_mode_args(sel)) {
        std::vector<std::string> args = {
            "-c",           "core.quotePath=false",
            "diff",         "--no-color",
            "--no-ext-diff", "-U0",
            "--diff-filter=d"};
        args.insert(args.end(), mode.begin(), mode.end());
        std::string blob;
        if (!run_git(args, blob, err)) return false;

        // Walk the unified diff: `+++ b/<path>` names the new side, each
        // `@@ -a,b +c,d @@` hunk adds lines c..c+d-1 (d omitted → 1; 0 → none).
        std::string cur;
        size_t pos = 0;
        while (pos < blob.size()) {
            size_t eol = blob.find('\n', pos);
            if (eol == std::string::npos) eol = blob.size();
            std::string_view line(blob.data() + pos, eol - pos);
            if (line.rfind("+++ b/", 0) == 0) {
                cur = prefix + std::string(line.substr(6));
            } else if (line.rfind("+++ ", 0) == 0) {
                cur.clear(); // "+++ /dev/null" or exotic prefix
            } else if (!cur.empty() && line.rfind("@@", 0) == 0) {
                size_t plus = line.find('+');
                if (plus != std::string_view::npos) {
                    uint32_t start = 0, count = 1;
                    size_t i = plus + 1;
                    while (i < line.size() && line[i] >= '0' && line[i] <= '9')
                        start = start * 10 + (line[i++] - '0');
                    if (i < line.size() && line[i] == ',') {
                        ++i;
                        count = 0;
                        while (i < line.size() && line[i] >= '0' && line[i] <= '9')
                            count = count * 10 + (line[i++] - '0');
                    }
                    auto &al = out[cur];
                    for (uint32_t L = start; L < start + count; ++L)
                        al.lines.push_back(L);
                }
            }
            pos = eol + 1;
        }
    }
    if (sel.untracked) {
        std::string blob;
        if (!run_git({"-c", "core.quotePath=false", "ls-files", "--others",
                      "--exclude-standard", "-z"},
                     blob, err))
            return false;
        std::vector<std::string> files;
        split_nul(blob, prefix, files);
        for (auto &f : files) out[f].whole_file = true;
    }
    for (auto &kv : out) {
        std::sort(kv.second.lines.begin(), kv.second.lines.end());
        kv.second.lines.erase(
            std::unique(kv.second.lines.begin(), kv.second.lines.end()),
            kv.second.lines.end());
    }
    return true;
}

bool git_churn(int days, std::unordered_map<std::string, uint32_t> &out,
               std::string &err) {
    std::string prefix;
    if (!repo_prefix(prefix, err)) return false;

    std::string since = "--since=" + std::to_string(days) + ".days.ago";
    std::string blob;
    if (!run_git({"-c", "core.quotePath=false", "log", since, "--name-only",
                  "--pretty=format:%H"},
                 blob, err))
        return false;

    // Walk the blob line by line without relying on git's blank-line
    // conventions between commits (those vary with --pretty=format:): any
    // line that's a bare 40- or 64-char hex string is a commit hash
    // (%H's exact width for SHA-1/SHA-256 repos), everything else
    // non-blank is a changed filename belonging to the commit above it.
    auto looks_like_hash = [](std::string_view s) {
        if (s.size() != 40 && s.size() != 64) return false;
        for (char c : s) {
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!hex) return false;
        }
        return true;
    };
    size_t pos = 0;
    while (pos < blob.size()) {
        size_t eol = blob.find('\n', pos);
        if (eol == std::string::npos) eol = blob.size();
        std::string_view line(blob.data() + pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || looks_like_hash(line)) continue;
        ++out[prefix + std::string(line)];
    }
    return true;
}

} // namespace hpr
