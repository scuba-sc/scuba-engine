// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// File implementations shared by the backends. Internal to the VFS.
#include "scuba/vfs/file_system.h"
#include "vfs/os_file.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace scuba::vfs::detail {

template <class Map>
std::vector<std::string_view> SortedPathViews(const Map& entries) {
    std::vector<std::string_view> paths;
    paths.reserve(entries.size());
    for (const auto& [path, value] : entries) {
        (void)value;
        paths.emplace_back(path);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

template <class Map, class SizeOf>
FileListPage BuildFileListPage(const Map& entries, const std::vector<std::string_view>& paths,
                               std::string_view prefix, std::string_view after, size_t limit, SizeOf sizeOf) {
    FileListPage page;
    if (limit == 0 || (!after.empty() && !prefix.empty() && !after.starts_with(prefix))) return page;

    auto it = std::lower_bound(paths.begin(), paths.end(), prefix,
                               [](std::string_view path, std::string_view key) { return path < key; });
    if (!after.empty()) {
        it = std::upper_bound(it, paths.end(), after,
                              [](std::string_view key, std::string_view path) { return key < path; });
    }

    page.entries.reserve(std::min(limit, paths.size()));
    for (; it != paths.end() && it->starts_with(prefix); ++it) {
        if (page.entries.size() == limit) {
            page.truncated = true;
            break;
        }
        const auto found = entries.find(std::string(*it));
        if (found == entries.end()) continue;  // The index is immutable; defensive against bad custom maps.
        page.entries.push_back({std::string(*it), static_cast<uint64_t>(sizeOf(found->second))});
    }
    return page;
}

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
