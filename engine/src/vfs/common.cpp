// SPDX-License-Identifier: GPL-3.0-only
// NormalizePath, ReadWhole, Crc32 and the memory backend.
#include "scuba/vfs/file_system.h"
#include "scuba/vfs/pack.h"
#include "vfs/files.h"

#include <array>
#include <map>

namespace scuba::vfs {

std::string NormalizePath(std::string_view in) {
    if (in.size() >= 2 && in[1] == ':') return {};   // "c:..." is never a VFS path
    std::string out;
    out.reserve(in.size());
    std::string seg;
    auto flush = [&]() {
        if (seg.empty()) return true;
        if (seg == "." || seg == "..") return false;
        if (!out.empty()) out += '/';
        out += seg;
        seg.clear();
        return true;
    };
    for (char c : in) {
        if (c == '\0') return {};
        if (c == '/' || c == '\\') {
            if (!flush()) return {};
            continue;
        }
        seg += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (!flush() || out.size() > kMaxPathLength) return {};
    return out;
}

Status ReadWhole(File& file, Bytes& out, uint64_t limit) {
    const uint64_t size = file.Size();
    if (size > limit) return Status::Fail("file is " + std::to_string(size) + " bytes, over the " + std::to_string(limit) + " limit");
    out.resize(static_cast<size_t>(size));
    if (size && file.ReadAt(0, out.data(), out.size()) != out.size()) {
        out.clear();
        return Status::Fail("short read");
    }
    return Status::Ok();
}

uint32_t Crc32(const void* data, size_t size, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

namespace {

class MemoryFs final : public FileSystem {
public:
    explicit MemoryFs(std::vector<std::pair<std::string, Bytes>> files) {
        for (auto& [path, bytes] : files) {
            const std::string norm = NormalizePath(path);
            if (!norm.empty()) files_[norm] = std::make_shared<const Bytes>(std::move(bytes));
        }
    }
    const char* Kind() const override { return "memory"; }
    bool Exists(std::string_view path) const override { return files_.count(std::string(path)) != 0; }
    std::unique_ptr<File> Open(std::string_view path, Status* why) const override {
        auto it = files_.find(std::string(path));
        if (it == files_.end()) {
            if (why) *why = Status::Fail("not found");
            return nullptr;
        }
        return std::make_unique<detail::MemoryFile>(it->second);
    }
    void List(std::string_view prefix, std::vector<Entry>& out) const override {
        for (const auto& [path, bytes] : files_)
            if (path.compare(0, prefix.size(), prefix) == 0) out.push_back({path, bytes->size()});
    }

private:
    std::map<std::string, std::shared_ptr<const Bytes>> files_;
};

}  // namespace

std::unique_ptr<FileSystem> MemoryFileSystem(std::vector<std::pair<std::string, Bytes>> files) {
    return std::make_unique<MemoryFs>(std::move(files));
}

}  // namespace scuba::vfs
