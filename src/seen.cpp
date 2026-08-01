#include "seen.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace hpr {

uint64_t fnv1a(std::string_view data) {
    uint64_t h = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ULL; // FNV-1a 64-bit prime
    }
    return h;
}

void SeenStore::load(const std::string &path) {
    std::ifstream in(path);
    if (!in) return; // missing/unreadable = empty store, not an error
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        // <file>\t<line_start>\t<line_end>\t<hash-in-hex>
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        size_t t3 = line.find('\t', t2 + 1);
        if (t3 == std::string::npos) continue;
        std::string file = line.substr(0, t1);
        SeenEntry e;
        char *endp = nullptr;
        e.line_start = static_cast<uint32_t>(
            std::strtoul(line.c_str() + t1 + 1, &endp, 10));
        if (endp != line.c_str() + t2) continue; // malformed — skip the line
        e.line_end = static_cast<uint32_t>(
            std::strtoul(line.c_str() + t2 + 1, &endp, 10));
        if (endp != line.c_str() + t3) continue;
        e.hash = std::strtoull(line.c_str() + t3 + 1, &endp, 16);
        if (endp != line.c_str() + line.size()) continue;
        prior_[file].push_back(e);
    }
}

bool SeenStore::seen_unchanged(const std::string &file, uint32_t line_start,
                               uint32_t line_end, uint64_t hash) const {
    auto it = prior_.find(file);
    if (it == prior_.end()) return false;
    for (const auto &e : it->second) {
        if (e.line_start == line_start && e.line_end == line_end)
            return e.hash == hash;
    }
    return false;
}

void SeenStore::mark(const std::string &file, uint32_t line_start,
                     uint32_t line_end, uint64_t hash) {
    current_[file].push_back({line_start, line_end, hash});
}

bool SeenStore::save(const std::string &path, std::string *err) const {
    std::ostringstream out;
    for (const auto &kv : current_) {
        for (const auto &e : kv.second) {
            out << kv.first << '\t' << e.line_start << '\t' << e.line_end
                << '\t' << std::hex << e.hash << std::dec << '\n';
        }
    }
    std::string data = out.str();

    std::filesystem::path target(path);
    std::filesystem::path dir =
        target.parent_path().empty() ? "." : target.parent_path();
    std::string tmpl = dir.string() + "/.hpr-seen." + target.filename().string() +
                       ".XXXXXX";
    std::vector<char> tmp(tmpl.begin(), tmpl.end());
    tmp.push_back('\0');
    int fd = ::mkstemp(tmp.data());
    if (fd < 0) {
        if (err) *err = std::string("cannot create temp file near ") + path +
                        ": " + std::strerror(errno);
        return false;
    }
    std::string tmp_path(tmp.data());
    bool ok = true;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (err) *err = std::string("write failed for ") + tmp_path + ": " +
                            std::strerror(errno);
            ok = false;
            break;
        }
        off += static_cast<size_t>(n);
    }
    if (ok) ::fchmod(fd, 0644);
    ::close(fd);
    if (ok && std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        if (err) *err = std::string("rename failed for ") + path + ": " +
                        std::strerror(errno);
        ok = false;
    }
    if (!ok) ::unlink(tmp_path.c_str());
    return ok;
}

} // namespace hpr
