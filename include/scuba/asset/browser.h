// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "scuba/asset/model.h"
#include "scuba/vfs/file_system.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scuba::asset {

enum class BrowserItemKind : uint8_t { Directory, File };

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
};

// Builds one level of VFS directories and files, or recursive file search results within the
// current directory. Directory entries are inferred from file paths because VFS listings expose
// files only. Unsupported file formats remain visible but are marked non-placeable.
Status BuildBrowserContents(std::span<const vfs::Entry> entries, std::string_view currentDirectory,
                            std::string_view search, BrowserContents& contents);

// Finds a visible file by its stable full VFS path. Directories and unsupported files can still be
// selected; the caller checks supportedModelFormat() before invoking the model loader.
const BrowserItem* FindBrowserFile(const BrowserContents& contents, std::string_view path) noexcept;

}  // namespace scuba::asset
