// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/asset/browser.h"

#include "scuba/vfs/file_system.h"

#include <algorithm>
#include <map>
#include <utility>

namespace scuba::asset {
namespace {

std::string LowerAscii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        result.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
    }
    return result;
}

bool IsWithin(std::string_view path, std::string_view directory, std::string_view& relative) {
    if (directory.empty()) {
        relative = path;
        return true;
    }
    if (path.size() <= directory.size() || path.compare(0, directory.size(), directory) != 0 ||
        path[directory.size()] != '/')
        return false;
    relative = path.substr(directory.size() + 1);
    return !relative.empty();
}

}  // namespace

Status BuildBrowserContents(std::span<const vfs::Entry> entries, std::string_view currentDirectory,
                            std::string_view search, BrowserContents& contents) {
    const std::string directory = vfs::NormalizePath(currentDirectory);
    if (!currentDirectory.empty() && directory.empty())
        return Status::Fail("asset browser directory is not a safe VFS path");

    std::map<std::string, BrowserItem> directories;
    std::map<std::string, BrowserItem> files;
    const std::string query = LowerAscii(search);
    for (const auto& entry : entries) {
        const std::string path = vfs::NormalizePath(entry.path);
        if (path.empty()) continue;
        std::string_view relative;
        if (!IsWithin(path, directory, relative)) continue;

        const size_t slash = relative.find('/');
        if (slash != std::string_view::npos) {
            const std::string folderName(relative.substr(0, slash));
            const std::string folderPath = directory.empty() ? folderName : directory + "/" + folderName;
            directories.emplace(folderPath, BrowserItem{BrowserItemKind::Directory, folderName,
                                                          folderPath, 0, ModelFormat::Unknown});
            if (!query.empty() && LowerAscii(relative).find(query) != std::string::npos) {
                files.emplace(path, BrowserItem{BrowserItemKind::File, std::string(relative), path,
                                                entry.size, FormatOf(path)});
            }
            continue;
        }

        if (!query.empty() && LowerAscii(relative).find(query) == std::string::npos) continue;
        files.emplace(path, BrowserItem{BrowserItemKind::File, std::string(relative), path,
                                        entry.size, FormatOf(path)});
    }

    BrowserContents candidate;
    candidate.directories.reserve(directories.size());
    candidate.files.reserve(files.size());
    for (auto& [path, item] : directories) {
        (void)path;
        candidate.directories.push_back(std::move(item));
    }
    for (auto& [path, item] : files) {
        (void)path;
        candidate.files.push_back(std::move(item));
    }
    contents = std::move(candidate);
    return Status::Ok();
}

const BrowserItem* FindBrowserFile(const BrowserContents& contents, std::string_view path) noexcept {
    const auto found = std::find_if(contents.files.begin(), contents.files.end(), [&](const BrowserItem& item) {
        return item.path == path;
    });
    return found == contents.files.end() ? nullptr : &*found;
}

}  // namespace scuba::asset
