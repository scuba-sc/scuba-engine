// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/vfs/file_system.h"
#include "vfs/files.h"

#include <optional>
#include <system_error>
#include <unordered_map>

namespace scuba::vfs {
namespace {

namespace fs = std::filesystem;

fs::path FromUtf8(std::string_view s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

class DirectoryFs final : public FileSystem {
public:
    struct Indexed {
        fs::path real;
        uint64_t size = 0;
    };

    DirectoryFs(fs::path root, std::unordered_map<std::string, Indexed> index)
        : root_(std::move(root)), index_(std::move(index)) {}

    const char* Kind() const override { return "dir"; }

    bool Exists(std::string_view path) const override {
        if (index_.count(std::string(path))) return true;
        std::error_code ec;
        return Probe(path, ec).has_value();
    }

    std::unique_ptr<File> Open(std::string_view path, Status* why) const override {
        fs::path real;
        if (auto it = index_.find(std::string(path)); it != index_.end()) {
            real = it->second.real;
        } else {
            std::error_code ec;
            auto probed = Probe(path, ec);
            if (!probed) {
                if (why) *why = Status::Fail("not found");
                return nullptr;
            }
            real = *probed;
        }
        auto os = detail::OsFile::Open(real, why);
        if (!os) return nullptr;
        const uint64_t size = os->Size();
        return std::make_unique<detail::SliceFile>(std::move(os), 0, size);
    }

    void List(std::string_view prefix, std::vector<Entry>& out) const override {
        for (const auto& [path, item] : index_)
            if (path.compare(0, prefix.size(), prefix) == 0) out.push_back({path, item.size});
    }

private:
    // A file created after the index was built. The path is normalized (no "..") and symlinks are
    // refused, so the probe cannot leave the root.
    std::optional<fs::path> Probe(std::string_view path, std::error_code& ec) const {
        const fs::path candidate = root_ / FromUtf8(path);
        if (fs::is_symlink(fs::symlink_status(candidate, ec)) || !fs::is_regular_file(candidate, ec)) return std::nullopt;
        return candidate;
    }

    fs::path root_;
    std::unordered_map<std::string, Indexed> index_;
};

}  // namespace

std::unique_ptr<FileSystem> OpenDirectory(const std::filesystem::path& root, Status& status, size_t maxEntries) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        status = Status::Fail("not a directory: " + root.generic_string());
        return nullptr;
    }
    std::unordered_map<std::string, DirectoryFs::Indexed> index;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto& entry = *it;
        std::error_code e2;
        if (entry.is_symlink(e2)) {
            if (entry.is_directory(e2)) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(e2)) continue;
        const auto rel = entry.path().lexically_relative(root).u8string();
        const std::string norm = NormalizePath(std::string_view(reinterpret_cast<const char*>(rel.data()), rel.size()));
        if (norm.empty()) continue;
        if (index.size() >= maxEntries) {
            status = Status::Fail("directory has more than " + std::to_string(maxEntries) + " files: " + root.generic_string());
            return nullptr;
        }
        index.emplace(norm, DirectoryFs::Indexed{entry.path(), entry.file_size(e2)});   // case collision: first wins
    }
    if (ec) {
        status = Status::Fail("cannot read directory " + root.generic_string() + ": " + ec.message());
        return nullptr;
    }
    status = Status::Ok();
    return std::make_unique<DirectoryFs>(root, std::move(index));
}

}  // namespace scuba::vfs
