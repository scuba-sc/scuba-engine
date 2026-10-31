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
        // Check before adding so a very large sparse file cannot wrap the total.
        if (size > kMaxBytes - report.bytes) return Status::Fail("distribution exceeds 256 MiB");
        report.bytes += size;
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
    for (const auto& path : {root / "data", plugins}) {
        if (fs::is_symlink(fs::symlink_status(path, ec)) || ec)
            return Status::Fail("symlink or inaccessible directory refused in product tree: " + path.generic_string());
    }
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
    const auto status = fs::symlink_status(out, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return Status::Fail("cannot inspect " + out.generic_string());
    if (status.type() == fs::file_type::not_found) return Status::Ok();
    if (fs::is_symlink(status)) return Status::Fail("symlink destination refused: " + out.generic_string());
    if (!fs::is_directory(out, ec) || ec)
        return Status::Fail("export destination exists and is not a directory: " + out.generic_string());
    if (!fs::is_empty(out, ec) || ec)
        return Status::Fail("export destination is not empty: " + out.generic_string());
    return Status::Ok();
}

// A deterministic sibling is convenient for diagnosis, but it belongs to this export only if
// create_directory succeeds. Never delete or reuse a path left by anyone else.
Status CreateStage(const fs::path& out, fs::path& staging) {
    staging = out.parent_path() / (out.filename().generic_string() + ".partial");
    std::error_code ec;
    fs::create_directories(out.parent_path().empty() ? fs::path(".") : out.parent_path(), ec);
    if (ec) return Status::Fail("cannot create destination parent: " + out.generic_string());
    if (!fs::create_directory(staging, ec) || ec)
        return Status::Fail("cannot create exclusive staging directory: " + staging.generic_string());
    return Status::Ok();
}

Status RefuseInsideData(const fs::path& root, const fs::path& out) {
    std::error_code ec;
    const auto data = fs::weakly_canonical(root / "data", ec);
    if (ec) return Status::Fail("cannot resolve product data directory");
    const auto dest = fs::weakly_canonical(out, ec);
    if (ec) return Status::Fail("cannot resolve export destination");
    auto a = data.begin(), b = dest.begin();
    for (; a != data.end() && b != dest.end() && *a == *b; ++a, ++b) {}
    if (a == data.end()) return Status::Fail("export destination is inside product data");
    return Status::Ok();
}

Status CopyData(const fs::path& root, const fs::path& staging, ExportReport& walked) {
    const fs::path data = root / "data";
    return Walk(data, 0, walked, [&](const fs::path& from) -> Status {
        std::error_code ec;
        const fs::path rel = fs::relative(from, data, ec);
        if (ec) return Status::Fail("cannot relativize " + from.generic_string());
        const fs::path to = staging / "data" / rel;
        fs::create_directories(to.parent_path(), ec);
        if (ec) return Status::Fail("cannot create " + to.parent_path().generic_string());
        if (!fs::copy_file(from, to, fs::copy_options::none, ec) || ec)
            return Status::Fail("cannot copy " + from.generic_string());
        return Status::Ok();
    });
}

}  // namespace

Status ExportProduct(const fs::path& root, const fs::path& out, ExportReport& report) {
    report = ExportReport{};
    if (auto st = ReadRoster(root, report); !st) return st;
    if (auto st = RefuseOccupied(out); !st) return st;
    if (auto st = RefuseInsideData(root, out); !st) return st;

    // Stage beside the destination before publication. As with cleanup, this assumes the caller
    // controls concurrent access to these paths; portable filesystem operations do not pin them.
    fs::path staging;
    std::error_code ec;
    if (auto st = CreateStage(out, staging); !st) return st;

    auto fail = [&](const Status& st) {
        std::error_code cleanup;
        fs::remove_all(staging, cleanup);
        return st;
    };

    ExportReport walked;
    if (auto st = CopyData(root, staging, walked); !st) return fail(st);

    // The roster has to survive the copy: a plugin folder that did not make it is a broken product.
    ExportReport staged;
    if (auto st = ReadRoster(staging, staged); !st) return fail(st);
    if (staged.plugins != report.plugins)
        return fail(Status::Fail("exported roster does not match the source roster"));

    if (!fs::exists(out, ec)) {
        std::error_code cec;
        fs::create_directories(out.parent_path().empty() ? fs::path(".") : out.parent_path(), cec);
        if (cec) return fail(Status::Fail("cannot create " + out.parent_path().generic_string()));
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
    if (ec) return fail(Status::Fail("cannot move the staged distribution into " + out.generic_string()));

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
    const auto status = fs::symlink_status(out, ec);
    if ((ec && ec != std::errc::no_such_file_or_directory) || status.type() != fs::file_type::not_found)
        return Status::Fail("export destination exists or cannot be inspected: " + out.generic_string());
    if (auto st = RefuseInsideData(root, out); !st) return st;
    fs::path staging;
    if (auto st = CreateStage(out, staging); !st) return st;
    auto fail = [&](const Status& st) {
        std::error_code cleanup;
        fs::remove_all(staging, cleanup);
        return st;
    };
    ExportReport walked;
    if (auto st = CopyData(root, staging, walked); !st) return fail(st);
    ExportReport staged;
    if (auto st = ReadRoster(staging, staged); !st) return fail(st);
    if (staged.plugins != report.plugins) return fail(Status::Fail("exported roster does not match the source roster"));
    const fs::path archive = staging / "product.scpak";
    if (auto st = vfs::WritePackFromDirectory(archive, staging); !st) return fail(st);
    if (auto st = vfs::VerifyPack(archive); !st)
        return fail(Status::Fail("packed distribution did not verify: " + st.error));
    ec.clear();
    // Linking in the same parent filesystem does not replace a destination that appeared since
    // preflight. A filesystem that cannot create the link fails closed.
    fs::create_hard_link(archive, out, ec);
    if (ec) return fail(Status::Fail("cannot publish packed distribution into " + out.generic_string()));
    std::error_code cleanup;
    fs::remove_all(staging, cleanup);
    report.files = walked.files;
    report.bytes = walked.bytes;
    log::Info("product", "exported " + out.generic_string() + ": " + std::to_string(report.files) + " file(s), " +
                             std::to_string(report.plugins.size()) + " plugin(s)");
    return Status::Ok();
}

}  // namespace scuba::product
