// SPDX-License-Identifier: GPL-3.0-only
// Product distribution export: the tree --product accepts again, and every refusal that keeps a
// half-written distribution from existing.
#include "check.h"
#include "scuba/product/export.h"
#include "scuba/vfs/file_system.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
        const auto st0 = ExportProduct(root, out, report);
        if (!st0) std::fprintf(stderr, "export refused: %s\n", st0.error.c_str());
        CHECK(st0);
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

    // A trailing separator still stages next to the destination and supports empty directories.
    for (const bool existing : {false, true}) {
        const fs::path out = base / (existing ? "trailing-empty" : "trailing-new");
        if (existing) fs::create_directories(out);
        ExportReport report;
        CHECK(ExportProduct(root, fs::path(out.string() + "/"), report));
        CHECK(fs::is_regular_file(out / "data" / "plugins" / "catalog_a" / "plugin.ini"));
        CHECK(!fs::exists(out.string() + ".partial"));
    }
    {
        const fs::path out = base / "trailing.scpak";
        ExportReport report;
        CHECK(ExportProductPack(root, fs::path(out.string() + "/"), report));
        CHECK(fs::is_regular_file(out));
        CHECK(!fs::exists(out.string() + ".partial"));
    }
    // A preexisting sibling stage must not be bypassed by a trailing separator.
    for (const bool packed : {false, true}) {
        const fs::path out = base / (packed ? "trailing-owned.scpak" : "trailing-owned");
        const fs::path stage = out.string() + ".partial";
        Write(stage / "keep.txt", "unrelated\n");
        ExportReport report;
        const fs::path withSeparator(out.string() + "/");
        CHECK(!(packed ? ExportProductPack(root, withSeparator, report)
                       : ExportProduct(root, withSeparator, report)));
        CHECK(!fs::exists(out));
        CHECK(fs::is_regular_file(stage / "keep.txt"));
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
        Write(root / "private-sibling.txt", "must not be exported\n");
        const fs::path pack = base / "dist.scpak";
        fs::remove(pack);
        Write(pack.string() + ".tmp", "unrelated\n");
        ExportReport report;
        CHECK(ExportProductPack(root, pack, report));
        CHECK(report.plugins.size() == 2);
        CHECK(fs::file_size(pack) > 0);
        CHECK(fs::file_size(pack.string() + ".tmp") == 10);
        scuba::Status openStatus;
        auto archive = scuba::vfs::OpenPack(pack, openStatus);
        CHECK(archive && openStatus);
        if (archive) {
            CHECK(archive->Exists("data/plugins/catalog_a/plugin.ini"));
            CHECK(!archive->Exists("private-sibling.txt"));
        }
        ExportReport again;
        const auto st = ExportProductPack(root, pack, again);
        CHECK(!st);
        CHECK(st.error.find("exists") != std::string::npos);
    }

    // Existing sibling staging paths are not owned by the exporter and must survive both paths.
    for (const bool packed : {false, true}) {
        const fs::path out = base / (packed ? "owned.scpak" : "owned");
        const fs::path stage = out.string() + ".partial";
        Write(stage / "keep.txt", "unrelated\n");
        ExportReport report;
        CHECK(!(packed ? ExportProductPack(root, out, report) : ExportProduct(root, out, report)));
        CHECK(fs::is_regular_file(stage / "keep.txt"));
        CHECK(!fs::exists(out));
    }

    // An unrelated staging file, or a link to someone else's directory, is not ours to clean up.
    {
        const fs::path out = base / "stage-file";
        const fs::path stage = out.string() + ".partial";
        Write(stage, "keep\n");
        ExportReport report;
        CHECK(!ExportProduct(root, out, report));
        CHECK(fs::is_regular_file(stage) && fs::file_size(stage) == 5);
        CHECK(!fs::exists(out));
    }
    {
        const fs::path out = base / "stage-link.scpak";
        const fs::path stage = out.string() + ".partial";
        const fs::path target = base / "unrelated";
        Write(target / "keep.txt", "keep\n");
        std::error_code ec;
        fs::create_directory_symlink(fs::absolute(target), stage, ec);
        if (!ec) {
            ExportReport report;
            CHECK(!ExportProductPack(root, out, report));
            CHECK(fs::is_symlink(fs::symlink_status(stage)));
            CHECK(fs::is_regular_file(target / "keep.txt"));
            CHECK(!fs::exists(out));
        }
    }

    // Refusal after staging is created removes only the owned stage and leaves the source intact.
    {
        const fs::path source = root / "data" / "oversize.bin";
        Write(source, "");
        std::error_code ec;
        fs::resize_file(source, (uint64_t{256} << 20) + 1, ec);
        if (!ec) {
            for (const bool packed : {false, true}) {
                const fs::path out = base / (packed ? "oversize.scpak" : "oversize");
                ExportReport report;
                CHECK(!(packed ? ExportProductPack(root, out, report) : ExportProduct(root, out, report)));
                CHECK(!fs::exists(out));
                CHECK(!fs::exists(out.string() + ".partial"));
                CHECK(fs::file_size(source) == (uint64_t{256} << 20) + 1);
            }
        }
        fs::remove(source);
    }

    // A destination within the input cannot be staged while traversing that input.
    {
        ExportReport report;
        CHECK(!ExportProduct(root, root / "data" / "nested", report));
        CHECK(!ExportProductPack(root, root / "data" / "nested.scpak", report));
        CHECK(!fs::exists(root / "data" / "nested.partial"));
        CHECK(!fs::exists(root / "data" / "nested.scpak.partial"));
    }

    // Concurrent exporters using the same destination must not share a staging tree or both
    // report success. This exercises cooperating exporters, not hostile path replacement.
    for (const bool packed : {false, true}) {
        for (int trial = 0; trial < 8; ++trial) {
            const fs::path out = base / ((packed ? "concurrent-pack-" : "concurrent-dir-") + std::to_string(trial));
            std::atomic<int> ready{0};
            std::atomic<bool> go{false};
            bool succeeded[2] = {false, false};
            auto run = [&](int index) {
                ExportReport report;
                ++ready;
                while (!go.load()) std::this_thread::yield();
                succeeded[index] = static_cast<bool>(packed ? ExportProductPack(root, out, report)
                                                           : ExportProduct(root, out, report));
            };
            std::thread first(run, 0), second(run, 1);
            while (ready.load() != 2) std::this_thread::yield();
            go = true;
            first.join();
            second.join();
            CHECK(succeeded[0] != succeeded[1]);
            CHECK(!fs::exists(out.string() + ".partial"));
            if (packed) {
                scuba::Status openStatus;
                auto archive = scuba::vfs::OpenPack(out, openStatus);
                CHECK(archive && openStatus);
                if (archive) CHECK(archive->Exists("data/plugins/catalog_a/plugin.ini"));
            } else {
                CHECK(fs::is_regular_file(out / "data" / "plugins" / "catalog_a" / "plugin.ini"));
            }
        }
    }

    // Refuse a symlink at the data root itself, not only symlinks below it.
    {
        const fs::path linkRoot = base / "linked-root";
        fs::create_directories(linkRoot);
        std::error_code ec;
        fs::create_directory_symlink(fs::absolute(root / "data"), linkRoot / "data", ec);
        if (!ec) {
            ExportReport report;
            CHECK(!ExportProduct(linkRoot, base / "linked-dist", report));
            CHECK(!ExportProductPack(linkRoot, base / "linked-dist.scpak", report));
        }
    }

    fs::remove_all(base);
    return CheckExit("product_export");
}
