// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/product/export.h"

#include "scuba/core/log.h"
#include "scuba/vfs/pack.h"

#include <algorithm>
#include <functional>
#include <system_error>

namespace scuba::product {
namespace {

namespace fs = std::filesystem;

// Same ceilings the profile snapshot uses: a distribution is a product tree, not an archive of the
// machine it was built on.
constexpr size_t kMaxFiles = 4096;
constexpr uint64_t kMaxBytes = uint64_t{256} << 20;
constexpr int kMaxDepth = 32;

Status Walk(const fs::path& dir, int depth, ExportReport& report,
            const std::function<Status(const fs::path&)>& onFile) {
    if (depth > kMaxDepth) return Status::Fail("distribution exceeds 32 directory levels: " + dir.generic_string());
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        const std::string name = path.generic_string();
        if (fs::is_symlink(path, ec) || ec)
            return Status::Fail("symlink refused in a product tree: " + name);
        if (fs::is_directory(path, ec)) {
            if (auto st = Walk(path, depth + 1, report, onFile); !st) return st;
            continue;
        }
        if (!fs::is_regular_file(path, ec) || ec)
            return Status::Fail("not a regular file in a product tree: " + name);
        const auto size = fs::file_size(path, ec);
        if (ec) return Status::Fail("cannot size " + name);
        if (++report.files > kMaxFiles) return Status::Fail("distribution exceeds 4096 files");
        report.bytes += size;
        if (report.bytes > kMaxBytes) return Status::Fail("distribution exceeds 256 MiB");
        if (onFile)
            if (auto st = onFile(path); !st) return st;
    }
    if (ec) return Status::Fail("cannot list " + dir.generic_string());
    return Status::Ok();
}

// The roster is the product: every folder under data/plugins must be a plugin that carries a
// readable manifest, because a distribution missing one is exactly the half-mounted product the
// product profile refuses at startup.
Status ReadRoster(const fs::path& root, ExportReport& report) {
    const fs::path plugins = root / "data" / "plugins";
    std::error_code ec;
    if (!fs::is_directory(plugins, ec) || ec)
        return Status::Fail("product requires " + plugins.generic_string());
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(plugins, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (fs::is_symlink(path, ec) || ec)
            return Status::Fail("symlink refused in the roster: " + path.generic_string());
        if (fs::is_directory(path, ec)) folders.push_back(path);
    }
    if (ec) return Status::Fail("cannot list " + plugins.generic_string());
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        std::error_code iec;
        if (!fs::is_regular_file(folder / "plugin.ini", iec) || iec)
            return Status::Fail("roster folder has no plugin.ini: " + folder.filename().generic_string());
        report.plugins.push_back(folder.filename().generic_string());
    }
    if (report.plugins.empty()) return Status::Fail("roster is empty: " + plugins.generic_string());
    return Status::Ok();
}

Status RefuseOccupied(const fs::path& out) {
    std::error_code ec;
    if (!fs::exists(out, ec)) return Status::Ok();
    if (ec) return Status::Fail("cannot inspect " + out.generic_string());
    if (!fs::is_directory(out, ec) || ec)
        return Status::Fail("export destination exists and is not a directory: " + out.generic_string());
    if (!fs::is_empty(out, ec) || ec)
        return Status::Fail("export destination is not empty: " + out.generic_string());
    return Status::Ok();
}

}  // namespace

Status ExportProduct(const fs::path& root, const fs::path& out, ExportReport& report) {
    report = ExportReport{};
    if (auto st = ReadRoster(root, report); !st) return st;
    if (auto st = RefuseOccupied(out); !st) return st;

    // Stage beside the destination and rename: a refused or interrupted export never leaves a
    // partial distribution where --product would find it.
    const fs::path staging = out.parent_path() / (out.filename().generic_string() + ".partial");
    std::error_code ec;
    fs::remove_all(staging, ec);   // our own leftover from an interrupted run
    if (!fs::create_directories(staging, ec) || ec)
        return Status::Fail("cannot create " + staging.generic_string());

    auto fail = [&](const Status& st) {
        std::error_code cleanup;
        fs::remove_all(staging, cleanup);
        return st;
    };

    ExportReport walked;
    const fs::path data = root / "data";
    if (auto st = Walk(data, 0, walked, [&](const fs::path& from) -> Status {
            std::error_code cec;
            const fs::path rel = fs::relative(from, data, cec);
            if (cec) return Status::Fail("cannot relativize " + from.generic_string());
            const fs::path to = staging / "data" / rel;
            fs::create_directories(to.parent_path(), cec);
            if (cec) return Status::Fail("cannot create " + to.parent_path().generic_string());
            if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, cec) || cec)
                return Status::Fail("cannot copy " + from.generic_string());
            return Status::Ok();
        });
        !st)
        return fail(st);

    // The roster has to survive the copy: a plugin folder that did not make it is a broken product.
    ExportReport staged;
    if (auto st = ReadRoster(staging, staged); !st) return fail(st);
    if (staged.plugins != report.plugins)
        return fail(Status::Fail("exported roster does not match the source roster"));

    if (!fs::exists(out, ec)) {
        if (!fs::create_directories(out.parent_path().empty() ? fs::path(".") : out.parent_path(), ec) || ec)
            return fail(Status::Fail("cannot create " + out.parent_path().generic_string()));
        fs::rename(staging, out, ec);
    } else {
        // An empty destination was allowed: move the staged tree into it.
        for (fs::directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code mec;
            fs::rename(it->path(), out / it->path().filename(), mec);
            if (mec) return fail(Status::Fail("cannot move " + it->path().generic_string()));
        }
        std::error_code cleanup;
        fs::remove_all(staging, cleanup);
    }
    if (ec) return Status::Fail("cannot move the staged distribution into " + out.generic_string());

    report.files = walked.files;
    report.bytes = walked.bytes;
    log::Info("product", "exported " + out.generic_string() + ": " + std::to_string(report.files) + " file(s), " +
                             std::to_string(report.bytes) + " byte(s), " + std::to_string(report.plugins.size()) +
                             " plugin(s)");
    return Status::Ok();
}

Status ExportProductPack(const fs::path& root, const fs::path& out, ExportReport& report) {
    report = ExportReport{};
    if (auto st = ReadRoster(root, report); !st) return st;
    std::error_code ec;
    if (fs::exists(out, ec)) return Status::Fail("export destination exists: " + out.generic_string());
    ExportReport walked;
    if (auto st = Walk(root, 0, walked, nullptr); !st) return st;
    if (auto st = vfs::WritePackFromDirectory(out, root); !st) return st;
    if (auto st = vfs::VerifyPack(out); !st) {
        std::error_code cleanup;
        fs::remove(out, cleanup);
        return Status::Fail("packed distribution did not verify: " + st.error);
    }
    report.files = walked.files;
    report.bytes = walked.bytes;
    log::Info("product", "exported " + out.generic_string() + ": " + std::to_string(report.files) + " file(s), " +
                             std::to_string(report.plugins.size()) + " plugin(s)");
    return Status::Ok();
}

}  // namespace scuba::product
