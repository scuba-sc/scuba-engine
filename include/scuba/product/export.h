// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Product distribution export: takes a mounted product root (`--product DIR`: `<DIR>/data/plugins`
// is the roster, `<DIR>/data` is the storage root) and writes a distribution that `--product`
// accepts again. The tree is staged next to the destination and renamed into place only after it
// is complete, so a refused or failed export never leaves a half-written distribution behind.
#include "scuba/core/status.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace scuba::product {

struct ExportReport {
    size_t files = 0;
    uint64_t bytes = 0;
    std::vector<std::string> plugins;   // roster folder names, sorted
};

// <root>/data is copied to <out>/data. Fail-closed: data/plugins must exist and carry at least one
// folder, every folder must have a readable plugin.ini, <out> must not exist or be an empty
// directory, and symlinks, special files or the size/entry/level limits refuse the whole export.
Status ExportProduct(const std::filesystem::path& root, const std::filesystem::path& out,
                     ExportReport& report);

// The same tree as one .scpak, paths relative to <root>, so `--product` can be pointed at a mount
// of it. Refuses an existing file.
Status ExportProductPack(const std::filesystem::path& root, const std::filesystem::path& out,
                         ExportReport& report);

}  // namespace scuba::product
