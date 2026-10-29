// SPDX-License-Identifier: GPL-3.0-only
// Product distribution export: the tree --product accepts again, and every refusal that keeps a
// half-written distribution from existing.
#include "check.h"
#include "scuba/product/export.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using scuba::product::ExportProduct;
using scuba::product::ExportProductPack;
using scuba::product::ExportReport;

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

// A product root with two data plugins and one file beside them.
fs::path MakeRoot(const fs::path& base) {
    fs::remove_all(base);
    const fs::path root = base / "product";
    for (const char* id : {"catalog_a", "catalog_b"}) {
        Write(root / "data" / "plugins" / id / "plugin.ini",
              std::string("id = ") + id + "\nname = " + id + "\nversion = 1.0.0\napi = 1.1\nkind = data\n");
        Write(root / "data" / "plugins" / id / "lists" / "ships.txt", "ship\n");
    }
    Write(root / "data" / "settings" / "ui.ini", "scale = 1\n");
    return root;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return CheckExit("product_export (missing scratch directory)");
    const fs::path base = argv[1];
    const fs::path root = MakeRoot(base);

    // Export the tree, then check what --product would find in it.
    {
        const fs::path out = base / "dist";
        fs::remove_all(out);
        ExportReport report;
        CHECK(ExportProduct(root, out, report));
        CHECK(report.plugins.size() == 2);
        CHECK(report.plugins[0] == "catalog_a" && report.plugins[1] == "catalog_b");
        CHECK(report.files == 5);   // two manifests, two lists, one settings file
        CHECK(fs::is_regular_file(out / "data" / "plugins" / "catalog_a" / "plugin.ini"));
        CHECK(fs::is_regular_file(out / "data" / "plugins" / "catalog_b" / "lists" / "ships.txt"));
        CHECK(fs::is_regular_file(out / "data" / "settings" / "ui.ini"));
        CHECK(!fs::exists(out.string() + ".partial"));

        // A second export into the same, now occupied, destination is refused.
        ExportReport again;
        const auto st = ExportProduct(root, out, again);
        CHECK(!st);
        CHECK(st.error.find("not empty") != std::string::npos);
    }

    // An empty destination directory is filled, not refused.
    {
        const fs::path out = base / "dist-empty";
        fs::remove_all(out);
        fs::create_directories(out);
        ExportReport report;
        CHECK(ExportProduct(root, out, report));
        CHECK(fs::is_regular_file(out / "data" / "plugins" / "catalog_a" / "plugin.ini"));
    }

    // A roster folder without a manifest is refused before anything is written.
    {
        fs::remove_all(root / "data" / "plugins" / "catalog_b" / "plugin.ini");
        const fs::path out = base / "dist-broken";
        fs::remove_all(out);
        ExportReport report;
        const auto st = ExportProduct(root, out, report);
        CHECK(!st);
        CHECK(st.error.find("catalog_b") != std::string::npos);
        CHECK(!fs::exists(out));
        Write(root / "data" / "plugins" / "catalog_b" / "plugin.ini",
              "id = catalog_b\nname = catalog_b\nversion = 1.0.0\napi = 1.1\nkind = data\n");
    }

    // No data/plugins at all is refused.
    {
        const fs::path bare = base / "bare";
        fs::remove_all(bare);
        fs::create_directories(bare / "data");
        ExportReport report;
        const auto st = ExportProduct(bare, base / "dist-bare", report);
        CHECK(!st);
        CHECK(st.error.find("plugins") != std::string::npos);
    }

    // An empty roster is refused.
    {
        const fs::path empty = base / "empty-roster";
        fs::remove_all(empty);
        fs::create_directories(empty / "data" / "plugins");
        ExportReport report;
        const auto st = ExportProduct(empty, base / "dist-empty-roster", report);
        CHECK(!st);
        CHECK(st.error.find("empty") != std::string::npos);
    }

    // One .scpak of the same tree, verified, and refused when the destination exists.
    {
        const fs::path pack = base / "dist.scpak";
        fs::remove(pack);
        ExportReport report;
        CHECK(ExportProductPack(root, pack, report));
        CHECK(report.plugins.size() == 2);
        CHECK(fs::file_size(pack) > 0);
        ExportReport again;
        const auto st = ExportProductPack(root, pack, again);
        CHECK(!st);
        CHECK(st.error.find("exists") != std::string::npos);
    }

    fs::remove_all(base);
    return CheckExit("product_export");
}
