// SPDX-License-Identifier: GPL-3.0-only
// .scpak reader, writer and verifier (layout in scuba/vfs/pack.h).
#include "scuba/vfs/pack.h"
#include "vfs/files.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_map>

namespace scuba::vfs {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kHeaderSize = 32;
constexpr uint64_t kAlign = 16;
constexpr uint64_t kMaxIndex = uint64_t{64} << 20;

template <typename T>
T Get(const uint8_t* p) {
    T v{};
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(static_cast<T>(p[i]) << (8 * i));
    return v;
}
template <typename T>
void Put(Bytes& out, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

struct PackEntry {
    uint64_t offset = 0, size = 0;
    uint32_t crc = 0;
};

class PackFs final : public FileSystem {
public:
    PackFs(std::shared_ptr<detail::OsFile> os, std::unordered_map<std::string, PackEntry> entries)
        : os_(std::move(os)), entries_(std::move(entries)) {}
    const char* Kind() const override { return "scpak"; }
    bool Exists(std::string_view path) const override { return entries_.count(std::string(path)) != 0; }
    std::unique_ptr<File> Open(std::string_view path, Status* why) const override {
        auto it = entries_.find(std::string(path));
        if (it == entries_.end()) {
            if (why) *why = Status::Fail("not found");
            return nullptr;
        }
        return std::make_unique<detail::SliceFile>(os_, it->second.offset, it->second.size);
    }
    void List(std::string_view prefix, std::vector<Entry>& out) const override {
        for (const auto& [path, e] : entries_)
            if (path.compare(0, prefix.size(), prefix) == 0) out.push_back({path, e.size});
    }
    const std::unordered_map<std::string, PackEntry>& entries() const { return entries_; }
    const detail::OsFile& os() const { return *os_; }

private:
    std::shared_ptr<detail::OsFile> os_;
    std::unordered_map<std::string, PackEntry> entries_;
};

std::unique_ptr<PackFs> OpenPackImpl(const fs::path& archive, Status& status) {
    auto os = detail::OsFile::Open(archive, &status);
    if (!os) return nullptr;
    auto fail = [&](const std::string& reason) -> std::unique_ptr<PackFs> {
        status = Status::Fail(archive.filename().generic_string() + ": " + reason);
        return nullptr;
    };
    uint8_t header[kHeaderSize];
    if (!os->ReadExact(0, header, sizeof header)) return fail("too small for a .scpak");
    if (std::memcmp(header, kPackMagic, sizeof kPackMagic) != 0) return fail("not a .scpak (bad magic)");
    if (Get<uint32_t>(header + 8) != kPackVersion) return fail("unsupported .scpak version " + std::to_string(Get<uint32_t>(header + 8)));
    const uint32_t count = Get<uint32_t>(header + 12);
    const uint64_t indexOffset = Get<uint64_t>(header + 16), indexSize = Get<uint64_t>(header + 24);
    if (count > kPackMaxEntries) return fail("too many entries");
    if (indexSize > kMaxIndex || indexOffset < kHeaderSize || indexOffset + indexSize > os->Size() || indexOffset + indexSize < indexOffset)
        return fail("index out of range");

    Bytes index(static_cast<size_t>(indexSize));
    if (indexSize && !os->ReadExact(indexOffset, index.data(), index.size())) return fail("short read");
    std::unordered_map<std::string, PackEntry> entries;
    size_t p = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 2 > index.size()) return fail("truncated index");
        const uint16_t len = Get<uint16_t>(&index[p]);
        if (p + 2 + len + 8 + 8 + 4 + 1 > index.size()) return fail("truncated index");
        const std::string path(reinterpret_cast<const char*>(&index[p + 2]), len);
        p += 2 + len;
        PackEntry e;
        e.offset = Get<uint64_t>(&index[p]);
        e.size = Get<uint64_t>(&index[p + 8]);
        e.crc = Get<uint32_t>(&index[p + 16]);
        const uint8_t compression = index[p + 20];
        p += 21;
        if (compression != 0) return fail("entry " + path + ": compression " + std::to_string(compression) + " not supported");
        if (NormalizePath(path) != path) return fail("entry with a non-normalized path: " + path);
        if (e.offset < kHeaderSize || e.offset + e.size > indexOffset || e.offset + e.size < e.offset) return fail("entry " + path + " out of range");
        if (!entries.emplace(path, e).second) return fail("duplicate entry " + path);
    }
    if (p != index.size()) return fail("index has trailing bytes");
    status = Status::Ok();
    return std::make_unique<PackFs>(std::move(os), std::move(entries));
}

}  // namespace

std::unique_ptr<FileSystem> OpenPack(const fs::path& archive, Status& status) {
    return OpenPackImpl(archive, status);
}

Status WritePack(const fs::path& out, const std::vector<std::pair<std::string, Bytes>>& files) {
    if (files.size() > kPackMaxEntries) return Status::Fail("too many files for one .scpak");
    std::map<std::string, const Bytes*> sorted;
    for (const auto& [path, bytes] : files) {
        const std::string norm = NormalizePath(path);
        if (norm.empty() || norm.size() > 0xFFFF) return Status::Fail("cannot pack path: " + path);
        if (!sorted.emplace(norm, &bytes).second) return Status::Fail("duplicate path after normalization: " + norm);
    }

    const fs::path tmp = fs::path(out).concat(".tmp");
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return Status::Fail("cannot write " + tmp.generic_string());
    uint64_t pos = kHeaderSize;
    const char zeros[kHeaderSize] = {};
    f.write(zeros, kHeaderSize);
    Bytes index;
    for (const auto& [path, bytes] : sorted) {
        const uint64_t pad = (kAlign - pos % kAlign) % kAlign;
        f.write(zeros, static_cast<std::streamsize>(pad));
        pos += pad;
        f.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        Put<uint16_t>(index, static_cast<uint16_t>(path.size()));
        index.insert(index.end(), path.begin(), path.end());
        Put<uint64_t>(index, pos);
        Put<uint64_t>(index, bytes->size());
        Put<uint32_t>(index, Crc32(bytes->data(), bytes->size()));
        index.push_back(0);   // stored
        pos += bytes->size();
    }
    f.write(reinterpret_cast<const char*>(index.data()), static_cast<std::streamsize>(index.size()));
    Bytes header(kPackMagic, kPackMagic + sizeof kPackMagic);
    Put<uint32_t>(header, kPackVersion);
    Put<uint32_t>(header, static_cast<uint32_t>(sorted.size()));
    Put<uint64_t>(header, pos);
    Put<uint64_t>(header, index.size());
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    f.close();
    if (!f) return Status::Fail("write failed: " + tmp.generic_string());
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) return Status::Fail("cannot replace " + out.generic_string() + ": " + ec.message());
    return Status::Ok();
}

Status WritePackFromDirectory(const fs::path& out, const fs::path& dir) {
    Status status;
    auto source = OpenDirectory(dir, status);
    if (!source) return status;
    std::vector<Entry> listing;
    source->List("", listing);
    std::vector<std::pair<std::string, Bytes>> files;
    for (const auto& e : listing) {
        auto file = source->Open(e.path, &status);
        if (!file) return status;
        Bytes bytes;
        if (auto st = ReadWhole(*file, bytes, uint64_t{4} << 30); !st) return Status::Fail(e.path + ": " + st.error);
        files.emplace_back(e.path, std::move(bytes));
    }
    return WritePack(out, files);
}

Status VerifyPack(const fs::path& archive) {
    Status status;
    auto pack = OpenPackImpl(archive, status);
    if (!pack) return status;
    Bytes chunk(1 << 20);
    for (const auto& [path, e] : pack->entries()) {
        uint32_t crc = 0;
        for (uint64_t at = 0; at < e.size;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk.size(), e.size - at));
            if (!pack->os().ReadExact(e.offset + at, chunk.data(), n)) return Status::Fail(path + ": short read");
            crc = Crc32(chunk.data(), n, crc);
            at += n;
        }
        if (crc != e.crc) return Status::Fail(path + ": CRC mismatch");
    }
    return Status::Ok();
}

}  // namespace scuba::vfs
