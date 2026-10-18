// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The engine's virtual file system: a priority-ordered stack of mounted backends (directories,
// zip archives, .scpak packs, memory), with sco-core's game-file overrides layered on top.
//
//   path ──► mount stack (highest priority first; equal priority: last mounted first)
//              each mount: mountPoint prefix match ─► backend->Open(relative path)
//          ──► first backend that has the file wins (it shadows the ones below)
//          ──► sco::vfs override table (optional): if it mounts this path, the winning file
//              becomes the *base* of a composed virtual file (sco::vfs::Table::Open)
//
// Mounting and unmounting publish a new immutable stack; readers take a snapshot and never block
// on mounts, and an open handle keeps its backend alive after an unmount. All methods are
// thread-safe.
#include "scuba/vfs/file_system.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace sco::vfs { class MountTable; }

namespace scuba::vfs {

using MountId = uint32_t;   // 0 is never a valid id

struct MountOptions {
    std::string mountPoint;   // "" mounts at the root; "packs/ui" serves "packs/ui/<path>"
    int priority = 0;         // higher wins; equal priority: the later mount wins
    std::string label;        // for diagnostics; defaults to the backend kind
};

struct MountInfo {
    MountId id = 0;
    std::string mountPoint, label, kind;
    int priority = 0;
    uint64_t order = 0;       // mount sequence number
};

class VirtualFileSystem {
public:
    VirtualFileSystem();
    ~VirtualFileSystem();
    VirtualFileSystem(const VirtualFileSystem&) = delete;
    VirtualFileSystem& operator=(const VirtualFileSystem&) = delete;

    // 0 (and why) when fs is null or the mount point does not normalize.
    MountId Mount(std::unique_ptr<FileSystem> fs, MountOptions options, Status* why = nullptr);
    bool Unmount(MountId id);
    std::vector<MountInfo> Mounts() const;   // resolution order

    // sco-core game-file overrides applied above the stack. The table outlives this object or is
    // reset to nullptr first. Null (the default) serves stack files unchanged.
    void SetOverrides(const sco::vfs::MountTable* table) noexcept;

    bool Exists(std::string_view path) const;
    std::unique_ptr<File> Open(std::string_view path, Status* why = nullptr) const;
    Status ReadAll(std::string_view path, Bytes& out, uint64_t limit = uint64_t{256} << 20) const;
    // Union of every mount; a path served by a higher mount hides the same path below. Sorted.
    std::vector<Entry> List(std::string_view prefix = {}) const;
    // "<label>:<relative path>" of the mount that serves path, or "" when none does.
    std::string Resolve(std::string_view path) const;

private:
    struct Mounted;
    using Stack = std::vector<std::shared_ptr<const Mounted>>;
    std::shared_ptr<const Stack> Snapshot() const;

    mutable std::shared_mutex mutex_;
    std::shared_ptr<const Stack> stack_;
    MountId nextId_ = 1;
    uint64_t nextOrder_ = 1;
    std::atomic<const sco::vfs::MountTable*> overrides_{nullptr};
};

}  // namespace scuba::vfs
