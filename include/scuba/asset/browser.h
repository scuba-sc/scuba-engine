// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scuba/asset/model.h"
#include "scuba/vfs/file_system.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scuba::asset {

enum class BrowserItemKind : uint8_t { Directory, File };

inline constexpr size_t kMaxBrowserCatalogEntries = 65'536;
inline constexpr size_t kMaxBrowserFolders = 4'096;
inline constexpr size_t kMaxBrowserFiles = 4'096;
inline constexpr size_t kMaxBrowserSearchLength = 256;

struct BrowserItem {
    BrowserItemKind kind = BrowserItemKind::File;
    std::string name;  // Leaf name in a folder view; path relative to the current folder in search.
    std::string path;  // Stable normalized VFS path used for selection and placement.
    uint64_t size = 0;
    ModelFormat format = ModelFormat::Unknown;

    // The extension is recognized by the model decoder. The file may still fail decoding or
    // have unresolved dependencies; those outcomes come from ModelLoader when it is opened.
    bool supportedModelFormat() const noexcept {
        return kind == BrowserItemKind::File && format != ModelFormat::Unknown;
    }
};

struct BrowserContents {
    std::vector<BrowserItem> directories;
    std::vector<BrowserItem> files;
    size_t omittedDirectories = 0;
    size_t omittedFiles = 0;
};

// Builds one level of VFS directories and files, or recursive file search results within the
// current directory. Input must be the canonical sorted/unique result from VFS List. The browser
// refuses oversized or malformed catalogues and caps each returned row list. Directory entries
// are inferred from file paths because VFS listings expose files only. Unsupported file formats
// remain visible but are marked non-placeable.
Status BuildBrowserContents(std::span<const vfs::Entry> entries, std::string_view currentDirectory,
                            std::string_view search, BrowserContents& contents);

// Finds a visible file by its stable full VFS path. Unsupported files can still be selected; the
// caller checks supportedModelFormat() before invoking the model loader.
const BrowserItem* FindBrowserFile(const BrowserContents& contents, std::string_view path) noexcept;

}  // namespace scuba::asset
