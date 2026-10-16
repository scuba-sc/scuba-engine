// SPDX-License-Identifier: GPL-3.0-only
// Zip archives (PKWARE APPNOTE layout): end-of-central-directory, central directory, local headers.
#include "scuba/vfs/file_system.h"
#include "scuba/vfs/pack.h"
#include "vfs/files.h"

#include "miniz.h"

#include <algorithm>
#include <unordered_map>

namespace scuba::vfs {
namespace {

constexpr uint32_t kEocdSig = 0x06054b50, kCentralSig = 0x02014b50, kLocalSig = 0x04034b50;
constexpr uint64_t kMaxInflate = uint64_t{1} << 30;   // one entry, inflated in memory

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

struct ZipEntry {
    uint16_t method = 0, flags = 0;
    uint32_t crc = 0;
    uint64_t packed = 0, size = 0, localHeader = 0;
};

class ZipFs final : public FileSystem {
public:
    ZipFs(std::shared_ptr<detail::OsFile> os, std::unordered_map<std::string, ZipEntry> entries)
        : os_(std::move(os)), entries_(std::move(entries)) {}

    const char* Kind() const override { return "zip"; }
    bool Exists(std::string_view path) const override { return entries_.count(std::string(path)) != 0; }

    std::unique_ptr<File> Open(std::string_view path, Status* why) const override {
        auto fail = [&](std::string reason) -> std::unique_ptr<File> {
            if (why) *why = Status::Fail(std::move(reason));
            return nullptr;
        };
        auto it = entries_.find(std::string(path));
        if (it == entries_.end()) return fail("not found");
        const ZipEntry& e = it->second;
        if (e.flags & 1) return fail("encrypted zip entry");
        if (e.method != 0 && e.method != 8) return fail("zip compression method " + std::to_string(e.method) + " not supported");

        uint8_t local[30];
        if (!os_->ReadExact(e.localHeader, local, sizeof local) || U32(local) != kLocalSig) return fail("bad local header");
        const uint64_t data = e.localHeader + 30 + U16(local + 26) + U16(local + 28);
        if (data + e.packed > os_->Size()) return fail("entry data past the end of the archive");

        if (e.method == 0) {
            if (e.packed != e.size) return fail("stored entry sizes disagree");
            return std::make_unique<detail::SliceFile>(os_, data, e.size);
        }
        if (e.size > kMaxInflate) return fail("deflated entry larger than 1 GiB");
        Bytes packed(static_cast<size_t>(e.packed));
        if (!packed.empty() && !os_->ReadExact(data, packed.data(), packed.size())) return fail("short read");
        auto out = std::make_shared<Bytes>(static_cast<size_t>(e.size));
        const size_t got = tinfl_decompress_mem_to_mem(out->data(), out->size(), packed.data(), packed.size(), 0);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got != out->size()) return fail("inflate failed");
        if (Crc32(out->data(), out->size()) != e.crc) return fail("CRC mismatch");
        return std::make_unique<detail::MemoryFile>(std::move(out));
    }

    void List(std::string_view prefix, std::vector<Entry>& out) const override {
        for (const auto& [path, e] : entries_)
            if (path.compare(0, prefix.size(), prefix) == 0) out.push_back({path, e.size});
    }

private:
    std::shared_ptr<detail::OsFile> os_;
    std::unordered_map<std::string, ZipEntry> entries_;
};

}  // namespace

std::unique_ptr<FileSystem> OpenZip(const std::filesystem::path& archive, Status& status) {
    auto os = detail::OsFile::Open(archive, &status);
    if (!os) return nullptr;
    auto fail = [&](const std::string& reason) -> std::unique_ptr<FileSystem> {
        status = Status::Fail(archive.filename().generic_string() + ": " + reason);
        return nullptr;
    };
    const uint64_t size = os->Size();
    if (size < 22) return fail("too small for a zip");

    // The end record sits in the last 22 bytes plus at most a 64 KiB comment.
    const uint64_t tail = std::min<uint64_t>(size, 22 + 0xFFFF);
    Bytes buf(static_cast<size_t>(tail));
    if (!os->ReadExact(size - tail, buf.data(), buf.size())) return fail("short read");
    size_t at = buf.size() - 22 + 1;
    bool found = false;
    while (at-- > 0)
        if (U32(&buf[at]) == kEocdSig) { found = true; break; }
    if (!found) return fail("no end-of-central-directory record");
    const uint8_t* eocd = &buf[at];
    if (U16(eocd + 4) != 0 || U16(eocd + 6) != 0) return fail("multi-disk zip not supported");
    const uint16_t count = U16(eocd + 10);
    const uint32_t cdSize = U32(eocd + 12), cdOffset = U32(eocd + 16);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) return fail("zip64 not supported");
    if (uint64_t{cdOffset} + cdSize > size) return fail("central directory past the end");

    Bytes cd(cdSize);
    if (cdSize && !os->ReadExact(cdOffset, cd.data(), cd.size())) return fail("short read");
    std::unordered_map<std::string, ZipEntry> entries;
    size_t p = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || U32(&cd[p]) != kCentralSig) return fail("bad central directory entry " + std::to_string(i));
        const uint16_t nameLen = U16(&cd[p + 28]), extraLen = U16(&cd[p + 30]), commentLen = U16(&cd[p + 32]);
        if (p + 46 + nameLen + extraLen + commentLen > cd.size()) return fail("truncated central directory");
        ZipEntry e;
        e.flags = U16(&cd[p + 8]);
        e.method = U16(&cd[p + 10]);
        e.crc = U32(&cd[p + 16]);
        e.packed = U32(&cd[p + 20]);
        e.size = U32(&cd[p + 24]);
        e.localHeader = U32(&cd[p + 42]);
        if (e.packed == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu || e.localHeader == 0xFFFFFFFFu) return fail("zip64 entry not supported");
        const std::string_view name(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        p += 46 + nameLen + extraLen + commentLen;
        if (name.empty() || name.back() == '/') continue;   // directory record
        const std::string norm = NormalizePath(name);
        if (norm.empty()) return fail("entry with an unsafe path: " + std::string(name));
        entries.emplace(norm, e);
    }
    status = Status::Ok();
    return std::make_unique<ZipFs>(std::move(os), std::move(entries));
}

}  // namespace scuba::vfs
