// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// A read-only OS file with positional reads that are safe from several threads at once
// (Windows: overlapped offsets on a synchronous handle; POSIX: pread). Internal to the VFS.
#include "scuba/core/status.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace scuba::vfs::detail {

class OsFile {
public:
    static std::shared_ptr<OsFile> Open(const std::filesystem::path& path, Status* why);
    ~OsFile();
    OsFile(const OsFile&) = delete;
    OsFile& operator=(const OsFile&) = delete;

    uint64_t Size() const noexcept { return size_; }
    size_t ReadAt(uint64_t offset, void* dst, size_t n) const;
    // ReadAt that must return exactly n bytes.
    bool ReadExact(uint64_t offset, void* dst, size_t n) const { return ReadAt(offset, dst, n) == n; }

private:
    OsFile() = default;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    uint64_t size_ = 0;
};

}  // namespace scuba::vfs::detail
