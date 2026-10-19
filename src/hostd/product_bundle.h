// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <string>
namespace scuba::hostd {
// Export a new directory containing only the profile's data/plugins plus an
// exact SHA-256 inventory. No storage databases or host configuration is copied.
bool ExportProductBundle(const std::filesystem::path& profile,
                         const std::filesystem::path& destination, std::string& error);
}
