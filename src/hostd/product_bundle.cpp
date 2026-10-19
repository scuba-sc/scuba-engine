// SPDX-License-Identifier: GPL-3.0-only
#include "hostd/product_bundle.h"
#include "sco/net/sha2.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace scuba::hostd {
bool ExportProductBundle(const std::filesystem::path& profile,
                         const std::filesystem::path& destination, std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    try {
        const auto source = fs::canonical(profile / "data" / "plugins");
        const auto output = fs::weakly_canonical(destination);
        const auto relative = output.lexically_relative(source);
        if (destination.empty() || fs::exists(output) || relative.empty() ||
            (!relative.empty() && *relative.begin() != "..")) {
            error = "Export requires a new destination outside the plugin tree"; return false;
        }
        std::vector<fs::path> files;
        uintmax_t total = 0;
        for (const auto& item : fs::recursive_directory_iterator(source)) {
            const auto status = item.symlink_status();
            if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status))) {
                error = "Plugin bundle refuses links and special files"; return false;
            }
            if (!fs::is_regular_file(status)) continue;
            const auto name = item.path().lexically_relative(source).generic_string();
            if (name.find_first_of("\r\n") != std::string::npos || files.size() == 4096 ||
                item.file_size() > 64u * 1024u * 1024u || total + item.file_size() > 256u * 1024u * 1024u) {
                error = "Plugin bundle exceeds bounded file/name limits"; return false;
            }
            total += item.file_size(); files.push_back(item.path());
        }
        if (files.empty()) { error = "Plugin bundle is empty"; return false; }
        std::sort(files.begin(), files.end());
        if (!fs::create_directory(output)) { error = "Cannot reserve new bundle destination"; return false; }
        try {
            std::ofstream inventory(output / "SHA256SUMS", std::ios::binary);
            if (!inventory) throw std::runtime_error("Cannot create bundle inventory");
            for (const auto& file : files) {
                const auto path = fs::path("data") / "plugins" / file.lexically_relative(source);
                const auto target = output / path;
                fs::create_directories(target.parent_path());
                std::ifstream input(file, std::ios::binary);
                std::ofstream copy(target, std::ios::binary);
                if (!input || !copy) throw std::runtime_error("Cannot open bundle file");
                sco_sha256 hash; sco_sha256_init(&hash);
                std::array<char, 65536> bytes{};
                while (input) {
                    input.read(bytes.data(), bytes.size()); const auto n = input.gcount();
                    if (n) { copy.write(bytes.data(), n); sco_sha256_update(&hash, bytes.data(), static_cast<size_t>(n)); }
                }
                if (!input.eof() || !copy.flush()) throw std::runtime_error("Bundle copy failed");
                uint8_t digest[32]; sco_sha256_final(&hash, digest);
                constexpr char hex[] = "0123456789abcdef";
                for (const auto byte : digest) inventory << hex[byte >> 4] << hex[byte & 15];
                inventory << "  " << path.generic_string() << '\n';
            }
            if (!inventory.flush()) throw std::runtime_error("Bundle inventory write failed");
        } catch (...) { fs::remove_all(output); throw; }
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
}
