#include "file_io.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hpr {

MappedFile::~MappedFile() { release(); }

MappedFile::MappedFile(MappedFile &&o) noexcept
    : data_(o.data_), size_(o.size_), mmapped_(o.mmapped_), open_(o.open_) {
    o.data_ = nullptr;
    o.size_ = 0;
    o.mmapped_ = false;
    o.open_ = false;
}

MappedFile &MappedFile::operator=(MappedFile &&o) noexcept {
    if (this != &o) {
        release();
        data_ = o.data_;
        size_ = o.size_;
        mmapped_ = o.mmapped_;
        open_ = o.open_;
        o.data_ = nullptr;
        o.size_ = 0;
        o.mmapped_ = false;
        o.open_ = false;
    }
    return *this;
}

void MappedFile::release() {
    if (mmapped_ && data_ && size_) {
        ::munmap(const_cast<char *>(data_), size_);
    }
    data_ = nullptr;
    size_ = 0;
    mmapped_ = false;
    open_ = false;
}

void MappedFile::close() { release(); }

bool MappedFile::open(const std::string &path) {
    release();
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0) { ::close(fd); return false; }
    if (!S_ISREG(st.st_mode)) { ::close(fd); return false; }
    if (st.st_size == 0) {
        ::close(fd);
        data_ = "";
        size_ = 0;
        mmapped_ = false;
        open_ = true;
        return true;
    }
    void *p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                     MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        return false;
    }
    // Hint sequential access — Vectorscan walks the buffer once start to end.
    ::posix_madvise(p, static_cast<size_t>(st.st_size), POSIX_MADV_SEQUENTIAL);
    data_ = static_cast<const char *>(p);
    size_ = static_cast<size_t>(st.st_size);
    mmapped_ = true;
    open_ = true;
    return true;
}

bool read_stdin(std::string &out) {
    out.clear();
    char buf[64 * 1024];
    while (true) {
        ssize_t n = ::read(0, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return true;
}

bool read_path_list(const std::string &src, bool nul,
                    std::vector<std::string> &out, std::string *err) {
    std::string content;
    if (src == "-") {
        if (!read_stdin(content)) {
            if (err) *err = "failed to read file list from stdin";
            return false;
        }
    } else {
        MappedFile mf;
        if (!mf.open(src)) {
            if (err) *err = "cannot read file list: " + src;
            return false;
        }
        content.assign(mf.view().data(), mf.view().size());
    }
    const char sep = nul ? '\0' : '\n';
    size_t start = 0;
    for (size_t i = 0; i <= content.size(); ++i) {
        if (i == content.size() || content[i] == sep) {
            size_t len = i - start;
            if (!nul && len > 0 && content[start + len - 1] == '\r') --len;
            if (len > 0) out.emplace_back(content, start, len);
            start = i + 1;
        }
    }
    return true;
}

} // namespace hpr
