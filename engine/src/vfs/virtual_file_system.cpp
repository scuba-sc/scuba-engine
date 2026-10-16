// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/vfs/virtual_file_system.h"

#include "sco/vfs.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace scuba::vfs {

struct VirtualFileSystem::Mounted {
    MountId id = 0;
    uint64_t order = 0;
    std::string mountPoint, label;
    int priority = 0;
    std::shared_ptr<FileSystem> fs;
};

namespace {

// path relative to a mount point, or nullopt when the mount doesn't cover it. A mount point
// itself is a directory, never a file.
std::optional<std::string_view> Relative(std::string_view path, const std::string& mountPoint) {
    if (mountPoint.empty()) return path.empty() ? std::nullopt : std::optional<std::string_view>(path);
    if (path.size() <= mountPoint.size() + 1 || path.compare(0, mountPoint.size(), mountPoint) != 0 ||
        path[mountPoint.size()] != '/')
        return std::nullopt;
    return path.substr(mountPoint.size() + 1);
}

// Keeps the backend alive for as long as a handle it opened is.
class HeldFile final : public File {
public:
    HeldFile(std::shared_ptr<FileSystem> owner, std::unique_ptr<File> inner)
        : owner_(std::move(owner)), inner_(std::move(inner)) {}
    uint64_t Size() const override { return inner_->Size(); }
    size_t ReadAt(uint64_t offset, void* dst, size_t n) override { return inner_->ReadAt(offset, dst, n); }
    File& inner() { return *inner_; }

private:
    std::shared_ptr<FileSystem> owner_;
    std::unique_ptr<File> inner_;
};

// sco::vfs reads the base through a cursor; our files are positional.
class CursorOver final : public sco::vfs::BaseIo {
public:
    explicit CursorOver(File& file) : file_(file) {}
    bool Seek(uint64_t pos) override {
        pos_ = pos;
        return true;
    }
    size_t Read(void* dst, size_t n) override {
        const size_t got = file_.ReadAt(pos_, dst, n);
        pos_ += got;
        return got;
    }

private:
    File& file_;
    uint64_t pos_ = 0;
};

// A stack file with sco-core overrides composed over it.
class OverlayFile final : public File {
public:
    OverlayFile(std::unique_ptr<File> base, std::shared_ptr<const sco::vfs::Composed> composed)
        : base_(std::move(base)), cursor_(*base_), reader_(std::move(composed)) {}
    uint64_t Size() const override { return reader_.Size(); }
    size_t ReadAt(uint64_t offset, void* dst, size_t n) override {
        if (offset > static_cast<uint64_t>(INT64_MAX) || !reader_.Seek(static_cast<int64_t>(offset), sco::vfs::Whence::Set)) return 0;
        return reader_.Read(dst, n, cursor_);
    }

private:
    std::unique_ptr<File> base_;
    CursorOver cursor_;
    sco::vfs::Reader reader_;
};

}  // namespace

VirtualFileSystem::VirtualFileSystem() : stack_(std::make_shared<const Stack>()) {}
VirtualFileSystem::~VirtualFileSystem() = default;

std::shared_ptr<const VirtualFileSystem::Stack> VirtualFileSystem::Snapshot() const {
    std::shared_lock<std::shared_mutex> hold(mutex_);
    return stack_;
}

MountId VirtualFileSystem::Mount(std::unique_ptr<FileSystem> fs, MountOptions options, Status* why) {
    if (!fs) {
        if (why) *why = Status::Fail("no file system to mount");
        return 0;
    }
    const std::string mountPoint = NormalizePath(options.mountPoint);
    if (!options.mountPoint.empty() && mountPoint.empty()) {
        if (why) *why = Status::Fail("invalid mount point: " + options.mountPoint);
        return 0;
    }
    auto m = std::make_shared<Mounted>();
    m->mountPoint = mountPoint;
    m->priority = options.priority;
    m->label = options.label.empty() ? fs->Kind() : options.label;
    m->fs = std::move(fs);

    std::unique_lock<std::shared_mutex> hold(mutex_);
    m->id = nextId_++;
    m->order = nextOrder_++;
    const MountId id = m->id;
    auto next = std::make_shared<Stack>(*stack_);
    next->push_back(std::move(m));
    std::stable_sort(next->begin(), next->end(), [](const auto& a, const auto& b) {
        return a->priority != b->priority ? a->priority > b->priority : a->order > b->order;
    });
    stack_ = std::move(next);
    if (why) *why = Status::Ok();
    return id;
}

bool VirtualFileSystem::Unmount(MountId id) {
    std::unique_lock<std::shared_mutex> hold(mutex_);
    auto next = std::make_shared<Stack>(*stack_);
    const auto before = next->size();
    next->erase(std::remove_if(next->begin(), next->end(), [id](const auto& m) { return m->id == id; }), next->end());
    if (next->size() == before) return false;
    stack_ = std::move(next);
    return true;
}

std::vector<MountInfo> VirtualFileSystem::Mounts() const {
    std::vector<MountInfo> out;
    for (const auto& m : *Snapshot())
        out.push_back({m->id, m->mountPoint, m->label, m->fs->Kind(), m->priority, m->order});
    return out;
}

void VirtualFileSystem::SetOverrides(const sco::vfs::MountTable* table) noexcept {
    overrides_.store(table, std::memory_order_release);
}

bool VirtualFileSystem::Exists(std::string_view path) const {
    const std::string norm = NormalizePath(path);
    if (norm.empty()) return false;
    for (const auto& m : *Snapshot())
        if (auto rel = Relative(norm, m->mountPoint); rel && m->fs->Exists(*rel)) return true;
    return false;
}

std::unique_ptr<File> VirtualFileSystem::Open(std::string_view path, Status* why) const {
    const std::string norm = NormalizePath(path);
    if (norm.empty()) {
        if (why) *why = Status::Fail("invalid path: " + std::string(path));
        return nullptr;
    }
    for (const auto& m : *Snapshot()) {
        auto rel = Relative(norm, m->mountPoint);
        if (!rel) continue;
        Status st;
        auto file = m->fs->Open(*rel, &st);
        if (!file) {
            // A higher mount that has the file but can't serve it is an error, never a silent
            // fall-through to a shadowed copy below.
            if (m->fs->Exists(*rel)) {
                if (why) *why = Status::Fail(m->label + ":" + std::string(*rel) + ": " + st.error);
                return nullptr;
            }
            continue;
        }
        std::unique_ptr<File> held = std::make_unique<HeldFile>(m->fs, std::move(file));
        if (const auto* table = overrides_.load(std::memory_order_acquire)) {
            if (auto snap = table->Current(); snap && snap->Mounted(norm)) {
                CursorOver probe(*held);
                if (auto composed = snap->Open(norm, probe, held->Size()))
                    held = std::make_unique<OverlayFile>(std::move(held), std::move(composed));
            }
        }
        if (why) *why = Status::Ok();
        return held;
    }
    if (why) *why = Status::Fail("not found: " + norm);
    return nullptr;
}

Status VirtualFileSystem::ReadAll(std::string_view path, Bytes& out, uint64_t limit) const {
    Status st;
    auto file = Open(path, &st);
    if (!file) return st;
    if (auto read = ReadWhole(*file, out, limit); !read) return Status::Fail(std::string(path) + ": " + read.error);
    return Status::Ok();
}

std::vector<Entry> VirtualFileSystem::List(std::string_view prefix) const {
    std::string pre = NormalizePath(prefix);
    if (!prefix.empty() && pre.empty()) return {};
    std::map<std::string, uint64_t> seen;   // first insert = highest mount = the one served
    std::vector<Entry> part;
    for (const auto& m : *Snapshot()) {
        std::string inner;   // prefix in the backend's own namespace
        const std::string& mp = m->mountPoint;
        if (mp.empty()) {
            inner = pre;
        } else if (pre.empty() || mp.compare(0, pre.size(), pre) == 0) {
            inner.clear();   // the whole mount lies under the prefix (or the prefix is partial)
        } else if (pre.size() > mp.size() && pre.compare(0, mp.size(), mp) == 0 && pre[mp.size()] == '/') {
            inner = pre.substr(mp.size() + 1);
        } else {
            continue;
        }
        part.clear();
        m->fs->List(inner, part);
        for (auto& e : part) {
            std::string full = mp.empty() ? std::move(e.path) : mp + "/" + e.path;
            if (full.compare(0, pre.size(), pre) == 0) seen.emplace(std::move(full), e.size);
        }
    }
    std::vector<Entry> out;
    out.reserve(seen.size());
    for (auto& [path, size] : seen) out.push_back({path, size});
    return out;
}

std::string VirtualFileSystem::Resolve(std::string_view path) const {
    const std::string norm = NormalizePath(path);
    if (norm.empty()) return {};
    for (const auto& m : *Snapshot())
        if (auto rel = Relative(norm, m->mountPoint); rel && m->fs->Exists(*rel)) return m->label + ":" + std::string(*rel);
    return {};
}

}  // namespace scuba::vfs
