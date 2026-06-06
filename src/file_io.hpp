// File slurping helpers.
//
// MappedFile uses mmap for regular files (zero-copy view, OS-managed paging),
// which is the fastest way to feed a buffer into Vectorscan. read_stdin reads
// stdin into a heap-grown string.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace hpr {

class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    MappedFile(MappedFile &&o) noexcept;
    MappedFile &operator=(MappedFile &&o) noexcept;

    // Map path read-only. Empty regular files yield a valid empty view.
    // Returns false on open/stat/mmap failure or if path is not a regular file.
    bool open(const std::string &path);
    void close();

    std::string_view view() const { return {data_, size_}; }
    bool is_open() const { return open_; }

private:
    void release();

    const char *data_ = nullptr;
    std::size_t size_ = 0;
    bool mmapped_ = false;
    bool open_ = false;
};

bool read_stdin(std::string &out);

} // namespace hpr
