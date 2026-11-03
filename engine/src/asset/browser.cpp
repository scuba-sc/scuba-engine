// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/asset/browser.h"

#include "scuba/vfs/file_system.h"

#include <algorithm>
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

bool IsSafeBrowserPath(std::string_view path) {
    size_t at = 0;
    while (at < path.size()) {
        const auto first = static_cast<unsigned char>(path[at]);
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return false;
            ++at;
            continue;
        }

        uint32_t codepoint = 0;
        size_t continuationCount = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1fu;
            continuationCount = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0fu;
            continuationCount = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07u;
            continuationCount = 3;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (path.size() - at <= continuationCount) return false;
        for (size_t i = 1; i <= continuationCount; ++i) {
            const auto continuation = static_cast<unsigned char>(path[at + i]);
            if ((continuation & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6) | (continuation & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
        at += continuationCount + 1;
    }
    return true;
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
    if (entries.size() > kMaxBrowserCatalogEntries)
        return Status::Fail("asset browser catalogue exceeds its entry limit");
    if (currentDirectory.size() > vfs::kMaxPathLength)
        return Status::Fail("asset browser directory exceeds the VFS path limit");
    if (search.size() > kMaxBrowserSearchLength)
        return Status::Fail("asset browser search exceeds its length limit");
    const std::string directory = vfs::NormalizePath(currentDirectory);
    if (!currentDirectory.empty() &&
        (directory.empty() || directory != currentDirectory || !IsSafeBrowserPath(directory)))
        return Status::Fail("asset browser directory is not a safe VFS path");

    BrowserContents candidate;
    candidate.directories.reserve(std::min(kMaxBrowserFolders, entries.size()));
    candidate.files.reserve(std::min(kMaxBrowserFiles, entries.size()));
    const std::string query = LowerAscii(search);
    std::string_view previousPath;
    std::string lastFolderPath;
    for (const auto& entry : entries) {
        if (entry.path.size() > vfs::kMaxPathLength)
            return Status::Fail("asset browser catalogue contains a path over the VFS path limit");
        const std::string path = vfs::NormalizePath(entry.path);
        if (path.empty() || path != entry.path || !IsSafeBrowserPath(path))
            return Status::Fail("asset browser catalogue contains an invalid VFS path");
        if (!previousPath.empty() && path <= previousPath)
            return Status::Fail("asset browser catalogue is not sorted and unique");
        previousPath = entry.path;
        std::string_view relative;
        if (!IsWithin(path, directory, relative)) continue;

        const size_t slash = relative.find('/');
        if (slash != std::string_view::npos) {
            const std::string folderName(relative.substr(0, slash));
            const std::string folderPath = directory.empty() ? folderName : directory + "/" + folderName;
            if (folderPath != lastFolderPath) {
                lastFolderPath = folderPath;
                if (query.empty() || folderName.find(query) != std::string::npos) {
                    if (candidate.directories.size() < kMaxBrowserFolders) {
                        candidate.directories.push_back(BrowserItem{BrowserItemKind::Directory, folderName,
                                                                     folderPath, 0, ModelFormat::Unknown});
                    } else {
                        ++candidate.omittedDirectories;
                    }
                }
            }
            if (!query.empty() && relative.find(query) != std::string_view::npos) {
                if (candidate.files.size() < kMaxBrowserFiles) {
                    candidate.files.push_back(BrowserItem{BrowserItemKind::File, std::string(relative), path,
                                                           entry.size, FormatOf(path)});
                } else {
                    ++candidate.omittedFiles;
                }
            }
            continue;
        }

        if (!query.empty() && relative.find(query) == std::string_view::npos) continue;
        if (candidate.files.size() < kMaxBrowserFiles) {
            candidate.files.push_back(BrowserItem{BrowserItemKind::File, std::string(relative), path,
                                                   entry.size, FormatOf(path)});
        } else {
            ++candidate.omittedFiles;
        }
    }

    std::sort(candidate.directories.begin(), candidate.directories.end(),
              [](const BrowserItem& left, const BrowserItem& right) { return left.path < right.path; });
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
