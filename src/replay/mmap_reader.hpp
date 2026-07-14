#pragma once
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <cstddef>
#include <stdexcept>

class MmapReader {
    const uint8_t* base_ = nullptr;
    size_t size_ = 0;
public:
    explicit MmapReader(const char* path) {
        int fd = ::open(path, O_RDONLY);
        if (fd < 0) throw std::runtime_error("open failed");

        struct stat st {};
        if (::fstat(fd, &st) < 0) {
            ::close(fd);
            throw std::runtime_error("fstat failed");
        }
        size_ = static_cast<size_t>(st.st_size);

        void* m = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);

        /*close(fd) after mmap: the mapping holds its own reference. The fd is dead weight.*/
        ::close(fd);
        if (m == MAP_FAILED) throw std::runtime_error("mmap failed");

        /*MADV_SEQUENTIAL: tells kernel to aggressively read ahead and drop pages behind, streaming workload*/
        ::madvise(m, size_, MADV_SEQUENTIAL);
        base_ = static_cast<const uint8_t*>(m);
    }

    ~MmapReader() { if (base_) ::munmap(const_cast<uint8_t*>(base_), size_); }

    MmapReader(const MmapReader&) = delete;
    MmapReader& operator=(const MmapReader&) = delete;

    const uint8_t* data() const noexcept { return base_; }
    size_t         size() const noexcept { return size_; }
};