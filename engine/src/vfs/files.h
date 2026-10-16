// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// File implementations shared by the backends. Internal to the VFS.
#include "scuba/vfs/file_system.h"
#include "vfs/os_file.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace scuba::vfs::detail {

// [offset, offset + size) of an OS file: a whole disk file, a stored zip entry, a pack entry.
class SliceFile final : public File {
public:
    SliceFile(std::shared_ptr<const OsFile> os, uint64_t offset, uint64_t size)
        : os_(std::move(os)), offset_(offset), size_(size) {}
    uint64_t Size() const override { return size_; }
    size_t ReadAt(uint64_t offset, void* dst, size_t n) override {
        if (offset >= size_) return 0;
        const size_t take = static_cast<size_t>(std::min<uint64_t>(n, size_ - offset));
        return os_->ReadAt(offset_ + offset, dst, take);
    }

private:
    std::shared_ptr<const OsFile> os_;
    uint64_t offset_, size_;
};

class MemoryFile final : public File {
public:
    explicit MemoryFile(std::shared_ptr<const Bytes> bytes) : bytes_(std::move(bytes)) {}
    uint64_t Size() const override { return bytes_->size(); }
    size_t ReadAt(uint64_t offset, void* dst, size_t n) override {
        if (offset >= bytes_->size()) return 0;
        const size_t take = std::min<size_t>(n, bytes_->size() - static_cast<size_t>(offset));
        std::memcpy(dst, bytes_->data() + offset, take);
        return take;
    }

private:
    std::shared_ptr<const Bytes> bytes_;
};

}  // namespace scuba::vfs::detail
