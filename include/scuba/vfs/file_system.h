// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Mountable file-system backends. A backend serves files by normalized path relative to its own
// root; VirtualFileSystem (virtual_file_system.h) stacks backends under mount points.
//
// Paths everywhere in scuba::vfs are normalized with NormalizePath: ASCII lowercase, '/'
// separators, no leading, trailing or repeated '/', no "." or ".." segments. Lookups are therefore
// case-insensitive on every platform, the same rule sco::vfs uses for game paths.
#include "scuba/core/status.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scuba::vfs {

using Bytes = std::vector<uint8_t>;

constexpr size_t kMaxPathLength = 1024;

// Empty when the path is empty, too long, absolute with a drive, or has a "." or ".." segment.
std::string NormalizePath(std::string_view path);

struct Entry {
    std::string path;   // normalized, relative to whatever was listed
    uint64_t size = 0;
};

// One open file. ReadAt is positional, so a handle never shares a cursor; still, one handle is
// used by one thread at a time. Open as many handles as there are concurrent readers.
class File {
public:
    virtual ~File() = default;
    virtual uint64_t Size() const = 0;
    // Copies up to n bytes starting at offset; fewer at the end of the file or on a read error.
    virtual size_t ReadAt(uint64_t offset, void* dst, size_t n) = 0;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual const char* Kind() const = 0;   // "dir", "zip", "scpak", "memory"
    virtual bool Exists(std::string_view path) const = 0;
    // Null when the file is missing or can't be served; why (optional) says which.
    virtual std::unique_ptr<File> Open(std::string_view path, Status* why = nullptr) const = 0;
    // Every file whose path starts with prefix ("" lists all). Appends; order unspecified.
    virtual void List(std::string_view prefix, std::vector<Entry>& out) const = 0;
};

// A directory on disk. The tree is indexed at open (symlinks skipped, at most maxEntries files),
// which gives case-insensitive lookup and List; files created later are still found by Open
// through a direct probe, but List shows them only after a remount.
std::unique_ptr<FileSystem> OpenDirectory(const std::filesystem::path& root, Status& status,
                                          size_t maxEntries = size_t{1} << 20);

// A zip archive: stored (0) and deflated (8) entries; encrypted, zip64 and multi-disk archives
// are refused at open with a reason. A deflated entry is inflated whole into memory at Open.
std::unique_ptr<FileSystem> OpenZip(const std::filesystem::path& archive, Status& status);

// A .scpak container (pack.h): scuba-engine's own read-optimized format, stored entries only.
std::unique_ptr<FileSystem> OpenPack(const std::filesystem::path& archive, Status& status);

// Files held in memory (tests, generated content, tools).
std::unique_ptr<FileSystem> MemoryFileSystem(std::vector<std::pair<std::string, Bytes>> files);

// Reads a whole file through a handle. Fails when it is larger than limit or comes up short.
Status ReadWhole(File& file, Bytes& out, uint64_t limit);

}  // namespace scuba::vfs
