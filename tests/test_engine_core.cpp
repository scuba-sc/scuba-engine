// SPDX-License-Identifier: GPL-3.0-only
// Native engine core: VFS backends and mount stack, sco::vfs overlay, job system, model decode and
// the async loader, picking. No window and no GPU.
#include "check.h"

#include "scuba/asset/model_loader.h"
#include "scuba/core/job_system.h"
#include "scuba/render/picking.h"
#include "scuba/render/transform_gizmo.h"
#include "scuba/scene/scene_document.h"
#include "scuba/vfs/pack.h"
#include "scuba/vfs/virtual_file_system.h"

#include "assets/formats.h"
#include "miniz.h"
#include "sco/vfs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

using namespace scuba;
namespace fs = std::filesystem;

namespace {

vfs::Bytes B(std::string_view s) { return vfs::Bytes(s.begin(), s.end()); }
std::string S(const vfs::Bytes& b) { return std::string(b.begin(), b.end()); }

std::string Read(const vfs::VirtualFileSystem& v, std::string_view path) {
    vfs::Bytes out;
    return v.ReadAll(path, out) ? S(out) : std::string("<error>");
}

class LegacyOnlyFileSystem final : public vfs::FileSystem {
public:
    const char* Kind() const override { return "legacy-test"; }
    bool Exists(std::string_view path) const override { return path == "legacy.txt"; }
    std::unique_ptr<vfs::File> Open(std::string_view, Status* why) const override {
        if (why) *why = Status::Fail("unused in legacy-list test");
        return nullptr;
    }
    void List(std::string_view prefix, std::vector<vfs::Entry>& out) const override {
        constexpr std::string_view path = "legacy.txt";
        if (path.starts_with(prefix)) out.push_back({std::string(path), 6});
    }
};

void WriteFile(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

fs::path TempRoot() {
    auto root = fs::temp_directory_path() / ("scuba-engine-core-test-" + std::to_string(std::rand()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

// ---- a minimal zip writer (stored and deflated entries) -------------------------------------------
void Le16(vfs::Bytes& b, uint32_t v) { b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); }
void Le32(vfs::Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }

struct ZipItem { std::string name; vfs::Bytes data; bool deflate = false; uint16_t flags = 0; };

vfs::Bytes MakeZip(const std::vector<ZipItem>& items) {
    vfs::Bytes out, cd;
    for (const auto& it : items) {
        vfs::Bytes payload = it.data;
        if (it.deflate) {
            size_t len = 0;
            void* p = tdefl_compress_mem_to_heap(it.data.data(), it.data.size(), &len, TDEFL_DEFAULT_MAX_PROBES);
            payload.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + len);
            mz_free(p);
        }
        const uint32_t crc = vfs::Crc32(it.data.data(), it.data.size());
        const uint32_t offset = static_cast<uint32_t>(out.size());
        Le32(out, 0x04034b50); Le16(out, 20); Le16(out, it.flags); Le16(out, it.deflate ? 8 : 0); Le16(out, 0); Le16(out, 0);
        Le32(out, crc); Le32(out, static_cast<uint32_t>(payload.size())); Le32(out, static_cast<uint32_t>(it.data.size()));
        Le16(out, static_cast<uint32_t>(it.name.size())); Le16(out, 0);
        out.insert(out.end(), it.name.begin(), it.name.end());
        out.insert(out.end(), payload.begin(), payload.end());
        Le32(cd, 0x02014b50); Le16(cd, 20); Le16(cd, 20); Le16(cd, it.flags); Le16(cd, it.deflate ? 8 : 0); Le16(cd, 0); Le16(cd, 0);
        Le32(cd, crc); Le32(cd, static_cast<uint32_t>(payload.size())); Le32(cd, static_cast<uint32_t>(it.data.size()));
        Le16(cd, static_cast<uint32_t>(it.name.size())); Le16(cd, 0); Le16(cd, 0); Le16(cd, 0); Le16(cd, 0); Le32(cd, 0); Le32(cd, offset);
        cd.insert(cd.end(), it.name.begin(), it.name.end());
    }
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), cd.begin(), cd.end());
    Le32(out, 0x06054b50); Le16(out, 0); Le16(out, 0);
    Le16(out, static_cast<uint32_t>(items.size())); Le16(out, static_cast<uint32_t>(items.size()));
    Le32(out, static_cast<uint32_t>(cd.size())); Le32(out, cdOffset); Le16(out, 0);
    return out;
}

void TestPaths() {
    CHECK(vfs::NormalizePath("\\Data\\Objects//Ship.CGF") == "data/objects/ship.cgf");
    CHECK(vfs::NormalizePath("/a/b/") == "a/b");
    CHECK(vfs::NormalizePath("a/../b").empty());
    CHECK(vfs::NormalizePath("./a").empty());
    CHECK(vfs::NormalizePath("c:/windows").empty());
    CHECK(vfs::NormalizePath("").empty());
    CHECK(vfs::NormalizePath(std::string(vfs::kMaxPathLength + 1, 'a')).empty());
}

void TestStack() {
    vfs::VirtualFileSystem v;
    Status st;
    const auto low = v.Mount(vfs::MemoryFileSystem({{"ui/a.txt", B("low a")}, {"ui/only-low.txt", B("low only")}}), {"", 0, "low"}, &st);
    CHECK(low != 0 && st.ok());
    const auto high = v.Mount(vfs::MemoryFileSystem({{"a.txt", B("high a")}}), {"UI", 10, "high"});
    CHECK(high != 0);
    CHECK(Read(v, "ui/a.txt") == "high a");                 // priority shadows
    CHECK(Read(v, "UI\\Only-Low.txt") == "low only");       // case-insensitive, falls through
    CHECK(v.Resolve("ui/a.txt") == "high:a.txt");
    CHECK(v.Exists("ui/a.txt") && !v.Exists("ui/missing.txt") && !v.Exists("ui"));
    const auto list = v.List("ui");
    CHECK(list.size() == 2 && list[0].path == "ui/a.txt" && list[0].size == 6);
    CHECK(v.Mounts().size() == 2 && v.Mounts()[0].label == "high");

    vfs::VfsListPage priorityPage;
    CHECK(v.ListPage("ui", "", 1, 0, priorityPage).ok());
    CHECK(priorityPage.entries.size() == 1 && priorityPage.entries[0].path == "ui/a.txt");
    CHECK(priorityPage.entries[0].size == 6);  // priority 10 shadows priority 0

    // Equal priority: the later mount wins.
    const auto later = v.Mount(vfs::MemoryFileSystem({{"ui/a.txt", B("later a")}}), {"", 10, "later"});
    CHECK(Read(v, "ui/a.txt") == "later a");

    // Bounded listing is sorted, unique, and uses the same winning mount as Open/List.
    vfs::VfsListPage firstPage;
    CHECK(v.ListPage("ui", "", 1, 0, firstPage).ok());
    CHECK(firstPage.entries.size() == 1 && firstPage.entries[0].path == "ui/a.txt");
    CHECK(firstPage.entries[0].size == 7 && firstPage.truncated);
    CHECK(firstPage.nextCursor == "ui/a.txt" && firstPage.snapshotId != 0);
    vfs::VfsListPage secondPage;
    CHECK(v.ListPage("ui", firstPage.nextCursor, 1, firstPage.snapshotId, secondPage).ok());
    CHECK(secondPage.entries.size() == 1 && secondPage.entries[0].path == "ui/only-low.txt");
    CHECK(secondPage.entries[0].size == 8 && !secondPage.truncated);
    CHECK(secondPage.snapshotId == firstPage.snapshotId);

    // A handle outlives its mount.
    auto handle = v.Open("ui/a.txt");
    CHECK(v.Unmount(later) && !v.Unmount(later));
    CHECK(handle && handle->Size() == 7);
    char buf[8] = {};
    CHECK(handle && handle->ReadAt(0, buf, 7) == 7 && std::string(buf, 7) == "later a");
    CHECK(Read(v, "ui/a.txt") == "high a");

    Status why;
    CHECK(!v.Open("ui/../secret", &why) && !why.ok());
    CHECK(v.Mount(vfs::MemoryFileSystem({}), {"../x", 0, ""}, &why) == 0 && !why.ok());
    CHECK(v.Mount(nullptr, {}, &why) == 0);

    // A mount change invalidates pagination rather than mixing two stack generations.
    vfs::VfsListPage stalePage;
    CHECK(!v.ListPage("ui", secondPage.nextCursor, 2, firstPage.snapshotId, stalePage).ok());
    CHECK(stalePage.entries.empty());
}

void TestPagedListing() {
    constexpr size_t kFiles = 5'003;
    std::vector<std::pair<std::string, vfs::Bytes>> files;
    files.reserve(kFiles + 3);
    for (size_t i = 0; i < kFiles; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "catalog/%05zu.item", i);
        files.emplace_back(name, B("x"));
    }
    files.emplace_back("catalogue/nearby.item", B("not in catalog"));
    files.emplace_back("other/catalog/also-nearby.item", B("not in catalog"));
    files.emplace_back("catalog/dir/nested.item", B("nested"));

    vfs::VirtualFileSystem v;
    CHECK(v.Mount(vfs::MemoryFileSystem(std::move(files)), {"", 0, "large-memory"}) != 0);

    std::vector<std::string> observed;
    std::string cursor;
    uint64_t snapshotId = 0;
    bool first = true;
    size_t pageCount = 0;
    for (;;) {
        vfs::VfsListPage page;
        const Status status = v.ListPage("catalog", cursor, 127, first ? 0 : snapshotId, page);
        CHECK(status.ok());
        if (!status) break;
        if (first) {
            snapshotId = page.snapshotId;
            first = false;
        }
        ++pageCount;
        CHECK(page.entries.size() <= 127);
        CHECK(page.snapshotId == snapshotId && snapshotId != 0);
        CHECK(page.nextCursor == (page.entries.empty() ? "" : page.entries.back().path));
        for (const auto& entry : page.entries) observed.push_back(entry.path);
        if (!page.truncated) break;
        CHECK(!page.entries.empty() && page.nextCursor != cursor);
        cursor = page.nextCursor;
    }
    CHECK(pageCount > 1 && pageCount <= 41);
    CHECK(observed.size() == kFiles + 1);  // includes the nested path, excludes catalogue/ sibling
    CHECK(std::is_sorted(observed.begin(), observed.end()));
    CHECK(std::adjacent_find(observed.begin(), observed.end()) == observed.end());
    CHECK(observed.front() == "catalog/00000.item");
    CHECK(observed.back() == "catalog/dir/nested.item");

    // Nested mounts must translate the full VFS cursor without restarting when the cursor
    // moves into a sibling prefix such as "a/z0-file" or "a/zebra/child".
    vfs::VirtualFileSystem nestedVfs;
    CHECK(nestedVfs.Mount(vfs::MemoryFileSystem({
        {"a/before", B("root-before")},
        {"a/z/one", B("root-one")},
        {"a/z/two", B("root-two")},
        {"a/z0-file", B("root-edge")},
        {"a/zz-last", B("root-last")},
        {"b/after", B("root-after")},
    }), {"", 0, "root"}) != 0);
    CHECK(nestedVfs.Mount(vfs::MemoryFileSystem({
        {"one", B("nested-one-wins")},
        {"two", B("nested-two-wins")},
    }), {"a/z", 10, "nested"}) != 0);
    CHECK(nestedVfs.Mount(vfs::MemoryFileSystem({
        {"child", B("sibling-child")},
    }), {"a/zebra", 5, "adjacent-sibling"}) != 0);

    const std::vector<std::string> expectedPaths = {
        "a/before", "a/z/one", "a/z/two", "a/z0-file", "a/zebra/child", "a/zz-last", "b/after"};
    std::vector<std::string> nestedObserved;
    std::string nestedCursor;
    uint64_t nestedSnapshotId = 0;
    for (size_t i = 0; i < expectedPaths.size() + 2; ++i) {
        vfs::VfsListPage nestedPage;
        const Status status = nestedVfs.ListPage("", nestedCursor, 1,
                                                  nestedSnapshotId, nestedPage);
        CHECK(status.ok());
        if (!status || nestedPage.entries.empty()) break;
        nestedSnapshotId = nestedPage.snapshotId;
        const std::string& path = nestedPage.entries.front().path;
        CHECK(nestedCursor.empty() || path > nestedCursor);
        nestedObserved.push_back(path);
        CHECK(nestedPage.nextCursor == path);
        if (!nestedPage.truncated) break;
        CHECK(nestedPage.nextCursor > nestedCursor);
        nestedCursor = nestedPage.nextCursor;
    }
    CHECK(nestedObserved == expectedPaths);

    // The higher-priority nested backend wins a path also present in the root backend.
    vfs::VfsListPage precedencePage;
    CHECK(nestedVfs.ListPage("a/z", "", 2, nestedSnapshotId, precedencePage).ok());
    CHECK(precedencePage.entries.size() == 2);
    CHECK(precedencePage.entries[0].path == "a/z/one");
    CHECK(precedencePage.entries[0].size == std::string("nested-one-wins").size());
    CHECK(precedencePage.entries[1].path == "a/z/two");
    CHECK(precedencePage.entries[1].size == std::string("nested-two-wins").size());

    // A root cursor after the nested tree must advance to the adjacent mount, while a cursor
    // within the nested directory advances in that directory's full-path coordinate system.
    vfs::VfsListPage afterSiblingPrefix;
    CHECK(nestedVfs.ListPage("", "a/z0-file", 1, nestedSnapshotId, afterSiblingPrefix).ok());
    CHECK(afterSiblingPrefix.entries.size() == 1);
    CHECK(afterSiblingPrefix.entries[0].path == "a/zebra/child");
    vfs::VfsListPage siblingDirectory;
    CHECK(nestedVfs.ListPage("a/zebra", "", 1, nestedSnapshotId, siblingDirectory).ok());
    CHECK(siblingDirectory.entries.size() == 1);
    CHECK(siblingDirectory.entries[0].path == "a/zebra/child");
    CHECK(!siblingDirectory.truncated);
    vfs::VfsListPage nestedFirst;
    CHECK(nestedVfs.ListPage("a/z", "", 1, nestedSnapshotId, nestedFirst).ok());
    CHECK(nestedFirst.entries.size() == 1 && nestedFirst.entries[0].path == "a/z/one");
    CHECK(nestedFirst.truncated);
    vfs::VfsListPage nestedSecond;
    CHECK(nestedVfs.ListPage("a/z", nestedFirst.nextCursor, 1, nestedSnapshotId, nestedSecond).ok());
    CHECK(nestedSecond.entries.size() == 1 && nestedSecond.entries[0].path == "a/z/two");
    CHECK(!nestedSecond.truncated);

    // Reject malformed or out-of-scope cursors and invalid page sizes without changing the output.
    vfs::VfsListPage untouched;
    untouched.nextCursor = "sentinel";
    CHECK(!v.ListPage("catalog/../escape", "", 10, 0, untouched).ok());
    CHECK(untouched.nextCursor == "sentinel");
    CHECK(!v.ListPage("catalog", "elsewhere/file", 10, 0, untouched).ok());
    CHECK(!v.ListPage("catalog", "catalog/../escape", 10, 0, untouched).ok());
    CHECK(!v.ListPage(std::string_view("catalog\0bad", 11), "", 10, 0, untouched).ok());
    CHECK(!v.ListPage("catalog", "", 0, 0, untouched).ok());
    CHECK(!v.ListPage("catalog", "", vfs::kMaxVfsListPageEntries + 1, 0, untouched).ok());

    // A one-entry page reports truncation only when a later matching unique path remains.
    vfs::VfsListPage one;
    CHECK(v.ListPage("catalog/dir", "", 1, 0, one).ok());
    CHECK(one.entries.size() == 1 && one.entries[0].path == "catalog/dir/nested.item");
    CHECK(!one.truncated && one.nextCursor == "catalog/dir/nested.item");

    // Existing custom FileSystem implementations remain source-compatible. Legacy callers keep
    // their old List path; bounded callers get an explicit error instead of an unbounded fallback.
    vfs::VirtualFileSystem legacyVfs;
    CHECK(legacyVfs.Mount(std::make_unique<LegacyOnlyFileSystem>(), {"", 0, "legacy"}) != 0);
    const auto legacyEntries = legacyVfs.List();
    CHECK(legacyEntries.size() == 1 && legacyEntries[0].path == "legacy.txt");
    vfs::VfsListPage unsupported;
    const Status unsupportedStatus = legacyVfs.ListPage("", "", 10, 0, unsupported);
    CHECK(!unsupportedStatus.ok());
    CHECK(unsupportedStatus.error.find("does not support bounded listing") != std::string::npos);
}

void TestDirectory(const fs::path& root) {
    const fs::path dir = root / "dir";
    WriteFile(dir / "Models" / "Ship.OBJ", "v 0 0 0\n");
    WriteFile(dir / "readme.txt", "hello");
    Status st;
    auto backend = vfs::OpenDirectory(dir, st);
    CHECK(backend && st.ok());
    vfs::VirtualFileSystem v;
    v.Mount(std::move(backend), {"content", 0, "dir"});
    CHECK(Read(v, "content/models/ship.obj") == "v 0 0 0\n");
    CHECK(v.List("content").size() == 2);
    vfs::VfsListPage page;
    CHECK(v.ListPage("content", "", 1, 0, page).ok());
    CHECK(page.entries.size() == 1 && page.entries[0].path == "content/models/ship.obj");
    CHECK(page.truncated);
    WriteFile(dir / "late.txt", "late");   // created after the index: found by the probe
    CHECK(Read(v, "content/late.txt") == "late");
    CHECK(!vfs::OpenDirectory(dir / "missing", st) && !st.ok());
}

void TestPack(const fs::path& root) {
    const fs::path pack = root / "test.scpak";
    std::vector<std::pair<std::string, vfs::Bytes>> files = {{"Models/A.bin", B("alpha")}, {"b.txt", B("bravo!")}, {"empty", {}}};
    CHECK(vfs::WritePack(pack, files).ok());
    CHECK(vfs::VerifyPack(pack).ok());
    Status st;
    auto backend = vfs::OpenPack(pack, st);
    CHECK(backend && st.ok());
    vfs::VirtualFileSystem v;
    v.Mount(std::move(backend), {"pak", 5, "test.scpak"});
    CHECK(Read(v, "pak/models/a.bin") == "alpha");
    CHECK(Read(v, "pak/b.txt") == "bravo!");
    CHECK(Read(v, "pak/empty").empty());
    CHECK(v.List("pak").size() == 3);
    vfs::VfsListPage page;
    CHECK(v.ListPage("pak", "", 2, 0, page).ok());
    CHECK(page.entries.size() == 2 && page.entries[0].path == "pak/b.txt" && page.entries[1].path == "pak/empty");
    CHECK(page.truncated);
    vfs::VfsListPage lastPage;
    CHECK(v.ListPage("pak", page.nextCursor, 2, page.snapshotId, lastPage).ok());
    CHECK(lastPage.entries.size() == 1 && lastPage.entries[0].path == "pak/models/a.bin");
    CHECK(!lastPage.truncated);

    CHECK(!vfs::WritePack(root / "dup.scpak", {{"a", B("1")}, {"A", B("2")}}).ok());
    CHECK(!vfs::WritePack(root / "bad.scpak", {{"../a", B("1")}}).ok());

    // Corrupt one data byte: the index still opens, VerifyPack catches it.
    {
        std::fstream f(pack, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(32);
        f.put('X');
    }
    CHECK(!vfs::VerifyPack(pack).ok());
    WriteFile(root / "junk.scpak", "not a pack at all, definitely not");
    CHECK(!vfs::OpenPack(root / "junk.scpak", st) && st.error.find("magic") != std::string::npos);
}

void TestZip(const fs::path& root) {
    std::string big;
    for (int i = 0; i < 2000; ++i) big += "scuba-engine deflate payload " + std::to_string(i % 7) + "\n";
    const auto zip = MakeZip({{"Docs/Stored.txt", B("stored bytes"), false, 0},
                              {"data/big.txt", B(big), true, 0},
                              {"dir/", {}, false, 0},
                              {"secret.bin", B("xx"), false, 1}});
    const fs::path path = root / "test.zip";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
    Status st;
    auto backend = vfs::OpenZip(path, st);
    CHECK(backend && st.ok());
    if (!backend) return;
    vfs::VirtualFileSystem v;
    v.Mount(std::move(backend), {"", 1, "test.zip"});
    CHECK(Read(v, "docs/stored.txt") == "stored bytes");
    CHECK(Read(v, "data/big.txt") == big);
    CHECK(v.List().size() == 3);   // the directory record is skipped
    vfs::VfsListPage page;
    CHECK(v.ListPage("", "", 2, 0, page).ok());
    CHECK(page.entries.size() == 2 && page.entries[0].path == "data/big.txt" && page.entries[1].path == "docs/stored.txt");
    CHECK(page.truncated);
    vfs::VfsListPage lastPage;
    CHECK(v.ListPage("", page.nextCursor, 2, page.snapshotId, lastPage).ok());
    CHECK(lastPage.entries.size() == 1 && lastPage.entries[0].path == "secret.bin");
    CHECK(!lastPage.truncated);
    Status why;
    CHECK(!v.Open("secret.bin", &why) && why.error.find("encrypted") != std::string::npos);
    WriteFile(root / "junk.zip", "PK but not really");
    CHECK(!vfs::OpenZip(root / "junk.zip", st));
}

void TestOverlay() {
    vfs::VirtualFileSystem v;
    v.Mount(vfs::MemoryFileSystem({{"data/table.bin", B("0123456789")}}), {});
    sco::vfs::Mount m;
    m.path = "Data\\Table.bin";
    sco::vfs::SpliceList splices;
    sco::vfs::Splice sp;
    sp.at = 2;
    sp.removed = 3;
    sp.bytes = std::make_shared<const sco::vfs::Bytes>(sco::vfs::Bytes{'A', 'B'});
    splices.splices.push_back(sp);
    m.producer = splices;
    sco::vfs::MountTable table;
    table.Publish(sco::vfs::Table::Build({m}));
    CHECK(Read(v, "data/table.bin") == "0123456789");
    v.SetOverrides(&table);
    CHECK(Read(v, "data/table.bin") == "01AB56789");
    auto f = v.Open("data/table.bin");
    CHECK(f && f->Size() == 9);
    char tail[4] = {};
    CHECK(f && f->ReadAt(5, tail, 4) == 4 && std::string(tail, 4) == "6789");
    v.SetOverrides(nullptr);
    CHECK(Read(v, "data/table.bin") == "0123456789");
}

void TestJobs() {
    JobSystem jobs(4);
    CHECK(jobs.workers() == 4);
    std::atomic<int> sum{0};
    for (int i = 1; i <= 1000; ++i) jobs.Submit([&sum, i] { sum += i; }, static_cast<JobPriority>(i % 3));
    jobs.Submit([] { throw std::runtime_error("expected test exception"); });
    jobs.WaitIdle();
    CHECK(sum == 500500);
    CHECK(jobs.queued() == 0);
    jobs.Submit([&sum] { sum = -1; });   // a worker survived the throw
    jobs.WaitIdle();
    CHECK(sum == -1);
}

assets::Mesh Triangle() {
    assets::Mesh m;
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 2, 0}};   // Y-up: the tip is "up"
    m.indices = {0, 1, 2};
    m.material = "tri";
    return m;
}

render::Float3 ApplyPoint(render::Float3 point, const render::Float4x4& matrix) {
    return {point.x * matrix.m[0][0] + point.y * matrix.m[1][0] + point.z * matrix.m[2][0] + matrix.m[3][0],
            point.x * matrix.m[0][1] + point.y * matrix.m[1][1] + point.z * matrix.m[2][1] + matrix.m[3][1],
            point.x * matrix.m[0][2] + point.y * matrix.m[1][2] + point.z * matrix.m[2][2] + matrix.m[3][2]};
}

render::Float3 ApplyVector(render::Float3 vector, const render::Float4x4& matrix) {
    return {vector.x * matrix.m[0][0] + vector.y * matrix.m[1][0] + vector.z * matrix.m[2][0],
            vector.x * matrix.m[0][1] + vector.y * matrix.m[1][1] + vector.z * matrix.m[2][1],
            vector.x * matrix.m[0][2] + vector.y * matrix.m[1][2] + vector.z * matrix.m[2][2]};
}

float Dot(render::Float3 a, render::Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A minimal hand-written ASCII FBX 7.4 scene: one triangle with explicit per-corner normals. The
// repository ships no third-party FBX asset, and ufbx reads ASCII FBX, so the test owns its bytes.
// yUp selects GlobalSettings UpAxis (1 = Y-up, 2 = Z-up) and puts the tip on that axis, so both
// variants must land with the tip at +Z in engine space.
vfs::Bytes Fbx(bool yUp) {
    static constexpr std::string_view kHead =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "}\n"
        "GlobalSettings:  {\n"
        "    Version: 1000\n"
        "    Properties70:  {\n"
        "        P: \"UpAxis\", \"int\", \"Integer\", \"\",";
    static constexpr std::string_view kMiddle =
        "\n"
        "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "    }\n"
        "}\n"
        "Objects:  {\n"
        "    Geometry: 100000, \"Geometry::tri\", \"Mesh\" {\n"
        "        Vertices: *9 {\n"
        "            a: 0,0,0,1,0,0,";
    static constexpr std::string_view kTail =
        "\n"
        "        }\n"
        "        PolygonVertexIndex: *3 {\n"
        "            a: 0,1,-3\n"
        "        }\n"
        "        GeometryVersion: 124\n"
        "        LayerElementNormal: 0 {\n"
        "            Version: 101\n"
        "            Name: \"\"\n"
        "            MappingInformationType: \"ByPolygonVertex\"\n"
        "            ReferenceInformationType: \"Direct\"\n"
        "            Normals: *9 {\n"
        "                a: ";
    static constexpr std::string_view kEnd =
        "\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    Model: 200000, \"Model::tri\", \"Mesh\" {\n"
        "        Version: 232\n"
        "    }\n"
        "}\n"
        "Connections:  {\n"
        "    C: \"OO\",100000,200000\n"
        "    C: \"OO\",200000,0\n"
        "}\n";
    std::string text(kHead);
    text += yUp ? "1" : "2";
    text += kMiddle;
    text += yUp ? "0,2,0" : "0,0,2";
    text += kTail;
    text += yUp ? "0,0,-1,0,0,-1,0,0,-1" : "0,-1,0,0,-1,0,0,-1,0";
    text += kEnd;
    return B(text);
}

void TestFbx() {
    for (const bool yUp : {true, false}) {
        asset::ModelData model;
        const auto bytes = Fbx(yUp);
        const auto st = asset::DecodeModel(yUp ? "y.fbx" : "z.fbx", bytes, nullptr, model);
        CHECK(st.ok());
        CHECK(model.meshes.size() == 1 && model.triangleCount() == 1);
        if (model.meshes.size() != 1) continue;
        const auto& m = model.meshes[0];
        CHECK(m.positions.size() == 9 && m.normals.size() == 9 && m.indices.size() == 3);
        CHECK(std::fabs(m.boundsMax[2] - 2.0f) < 1e-6f);   // the tip is +Z in engine space either way
        CHECK(std::fabs(m.boundsMax[0] - 1.0f) < 1e-6f);
        CHECK(m.material.empty() || m.material == "tri");
        // Y-up normals (0,0,-1) rotate to +Y; a Z-up file's (0,-1,0) is taken as is.
        CHECK(std::fabs(m.normals[1] - (yUp ? 1.0f : -1.0f)) < 1e-6f);
    }
    asset::ModelData broken;
    CHECK(!asset::DecodeModel("junk.fbx", B("fbx"), nullptr, broken).ok());
    CHECK(!asset::DecodeModel("junk.fbx", vfs::Bytes{}, nullptr, broken).ok());
}

void TestLoader() {
    vfs::VirtualFileSystem v;
    const auto glb = assets::MakeGlb({Triangle()});
    v.Mount(vfs::MemoryFileSystem({{"models/tri.glb", glb},
                                   {"models/tri.fbx", Fbx(true)},
                                   {"models/quad.obj", B("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n")},
                                   {"models/broken.glb", B("glTF junk")},
                                   {"models/thing.fbx", B("fbx")}}),
            {});
    JobSystem jobs(2);
    {
        asset::ModelLoader loader(v, jobs);
        const auto tri = loader.Request("Models/Tri.GLB");
        CHECK(loader.Request("models/tri.glb") == tri);   // deduplicated by normalized path
        const auto quad = loader.Request("models/quad.obj", JobPriority::High);
        const auto fbxOk = loader.Request("models/tri.fbx");
        const auto broken = loader.Request("models/broken.glb");
        const auto fbx = loader.Request("models/thing.fbx");
        const auto missing = loader.Request("models/missing.obj");
        const auto bad = loader.Request("../x.obj");
        jobs.WaitIdle();

        CHECK(loader.Info(tri).state == asset::LoadState::Ready);
        auto t = loader.Get(tri);
        CHECK(t && t->meshes.size() == 1 && t->triangleCount() == 1);
        if (t && t->meshes.size() == 1) {
            const auto& m = t->meshes[0];
            CHECK(std::fabs(m.positions[8] - 2.0f) < 1e-6f);    // Y-up tip became +Z
            CHECK(std::fabs(m.boundsMax[2] - 2.0f) < 1e-6f);
            CHECK(m.normals.size() == 9 && std::fabs(std::fabs(m.normals[1]) - 1.0f) < 1e-5f);   // generated, facing -Y/+Y
        }
        auto q = loader.Get(quad);
        CHECK(q && q->triangleCount() == 2);
        CHECK(loader.Info(fbxOk).state == asset::LoadState::Ready);
        auto f = loader.Get(fbxOk);
        CHECK(f && f->meshes.size() == 1 && f->triangleCount() == 1);
        if (f && f->meshes.size() == 1)
            CHECK(std::fabs(f->meshes[0].boundsMax[2] - 2.0f) < 1e-6f);   // Y-up FBX became Z-up
        CHECK(loader.Info(broken).state == asset::LoadState::Failed);
        CHECK(loader.Info(fbx).state == asset::LoadState::Failed && loader.Info(fbx).error.find("FBX") != std::string::npos);
        CHECK(loader.Info(missing).state == asset::LoadState::Failed);
        CHECK(loader.Info(bad).state == asset::LoadState::Failed);
        CHECK(loader.TakeCompleted().size() == 7);
        CHECK(loader.TakeCompleted().empty());
        CHECK(loader.Request("models/broken.glb") != broken);   // failures are retried
        jobs.WaitIdle();
    }

    // A scene node survives a missing-file failure. Once the asset is mounted, retrying the old
    // failed handle starts one shared request; repeated retries and ordinary requests reuse it.
    vfs::VirtualFileSystem lateVfs;
    JobSystem lateJobs(2);
    scene::Document retainedScene;
    retainedScene.id = "retry-scene";
    retainedScene.name = "Retry scene";
    scene::Node missingModel;
    missingModel.id = "node-retry";
    missingModel.name = "Recoverable model";
    missingModel.kind = scene::NodeKind::ModelInstance;
    missingModel.assetPath = "models/recover.glb";
    retainedScene.nodes.push_back(missingModel);
    CHECK(scene::Validate(retainedScene).ok());
    const auto retainedSnapshot = retainedScene;
    asset::ModelLoader retryLoader(lateVfs, lateJobs);
    const auto failedHandle = retryLoader.Request(missingModel.assetPath, JobPriority::High);
    lateJobs.WaitIdle();
    CHECK(retryLoader.Info(failedHandle).state == asset::LoadState::Failed);
    CHECK(!retryLoader.Get(failedHandle));
    const auto secondFailedHandle = retryLoader.Request(missingModel.assetPath, JobPriority::High);
    CHECK(secondFailedHandle != failedHandle);
    lateJobs.WaitIdle();
    const auto failedCompletions = retryLoader.TakeCompleted();
    CHECK(failedCompletions.size() == 2);
    CHECK(std::find(failedCompletions.begin(), failedCompletions.end(), failedHandle) != failedCompletions.end());
    CHECK(std::find(failedCompletions.begin(), failedCompletions.end(), secondFailedHandle) != failedCompletions.end());
    CHECK(retainedScene == retainedSnapshot && retainedScene.nodes[0].assetPath == missingModel.assetPath);
    CHECK(retryLoader.Retry(999999) == 0);  // unknown handles cannot create stray requests

    Status lateMountStatus;
    const auto recoverableGlb = assets::MakeGlb({Triangle()});
    CHECK(lateVfs.Mount(vfs::MemoryFileSystem({{missingModel.assetPath, recoverableGlb}}),
                         {"", 0, "late-assets"}, &lateMountStatus) != 0 && lateMountStatus.ok());
    const auto recoveredHandle = retryLoader.Retry(failedHandle, JobPriority::High);
    CHECK(recoveredHandle != 0 && recoveredHandle != failedHandle);
    CHECK(retryLoader.Retry(failedHandle, JobPriority::High) == recoveredHandle);
    CHECK(retryLoader.Retry(secondFailedHandle, JobPriority::High) == recoveredHandle);
    CHECK(retryLoader.Request(missingModel.assetPath, JobPriority::High) == recoveredHandle);
    lateJobs.WaitIdle();
    CHECK(retryLoader.Info(failedHandle).state == asset::LoadState::Failed);
    CHECK(retryLoader.Info(recoveredHandle).state == asset::LoadState::Ready);
    const auto recoveredModel = retryLoader.Get(recoveredHandle);
    CHECK(recoveredModel && recoveredModel->triangleCount() == 1);
    CHECK(retryLoader.TakeCompleted() == std::vector<asset::ModelHandle>{recoveredHandle});
    CHECK(retryLoader.Retry(recoveredHandle, JobPriority::High) == recoveredHandle);
    CHECK(retryLoader.Get(recoveredHandle) == recoveredModel);
    CHECK(retainedScene == retainedSnapshot);
}

void TestPicking() {
    asset::ModelData model;
    asset::MeshData cube;
    for (float z : {-1.0f, 1.0f})
        for (float y : {-1.0f, 1.0f})
            for (float x : {-1.0f, 1.0f}) cube.positions.insert(cube.positions.end(), {x, y, z});
    cube.indices = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 3, 7, 1, 7, 5};
    asset::FinishMesh(cube);
    model.meshes.push_back(cube);
    model.boundsMin = cube.boundsMin;
    model.boundsMax = cube.boundsMax;

    render::Transform aTransform, bTransform;
    bTransform.position = {10, 0, 0};
    bTransform.scale = 2;
    render::PickTarget a{1, &model, aTransform.Matrix()}, b{2, &model, bTransform.Matrix()};
    const render::PickTarget targets[] = {a, b};
    auto hit = render::Pick({{0, 0, 10}, {0, 0, -1}}, targets);
    CHECK(hit && hit->id == 1 && std::fabs(hit->distance - 9.0f) < 1e-4f);
    hit = render::Pick({{10, 0, 10}, {0, 0, -1}}, targets);
    CHECK(hit && hit->id == 2 && std::fabs(hit->distance - 8.0f) < 1e-4f);   // scaled cube top at z = 2
    CHECK(!render::Pick({{5, 0, 10}, {0, 0, -1}}, targets));
    auto ground = render::RayPlaneZ({{0, 0, 5}, {0, 0.6f, -0.8f}}, 0);
    CHECK(ground && std::fabs(ground->y - 3.75f) < 1e-4f);

    render::Float4x4 parent, child;
    CHECK(render::AffineFromTrs({4, 3, 1}, {0, 0, 0.70710678f, 0.70710678f}, {2, 1, 1}, parent));
    CHECK(render::AffineFromTrs({0.3f, -0.2f, 0.4f}, {0, 0, 0.25881904f, 0.96592583f}, {1.5f, 0.75f, 0.5f}, child));
    const render::Float4x4 world = render::Multiply(child, parent);  // non-uniform parent + child rotation introduces shear
    render::Float4x4 inverse, normal;
    CHECK(render::InvertAffine(world, inverse));
    CHECK(render::NormalMatrix(world, normal));
    const render::Float3 localPoint{0.2f, 0.1f, 1.0f};
    const auto worldPoint = ApplyPoint(localPoint, world);
    const auto recovered = ApplyPoint(worldPoint, inverse);
    CHECK(std::fabs(recovered.x - localPoint.x) < 1e-4f && std::fabs(recovered.y - localPoint.y) < 1e-4f &&
          std::fabs(recovered.z - localPoint.z) < 1e-4f);
    const render::Float3 localTangent{1, -1, 0}, localNormal{1, 1, 0};
    CHECK(std::fabs(Dot(ApplyVector(localTangent, world), ApplyVector(localNormal, normal))) < 1e-4f);

    std::array<float, 3> worldMin{}, worldMax{};
    CHECK(render::TransformBounds({-1, -1, -1}, {1, 1, 1}, world, worldMin, worldMax));
    for (unsigned corner = 0; corner < 8; ++corner) {
        const render::Float3 point = ApplyPoint({(corner & 1u) ? 1.0f : -1.0f,
                                                 (corner & 2u) ? 1.0f : -1.0f,
                                                 (corner & 4u) ? 1.0f : -1.0f}, world);
        CHECK(point.x >= worldMin[0] - 1e-5f && point.x <= worldMax[0] + 1e-5f);
        CHECK(point.y >= worldMin[1] - 1e-5f && point.y <= worldMax[1] + 1e-5f);
        CHECK(point.z >= worldMin[2] - 1e-5f && point.z <= worldMax[2] + 1e-5f);
    }

    const render::PickTarget nestedTarget{3, &model, world};
    const auto nestedHit = render::Pick({{worldPoint.x, worldPoint.y, worldPoint.z + 10}, {0, 0, -2}},
                                        std::span<const render::PickTarget>(&nestedTarget, 1));
    CHECK(nestedHit && nestedHit->id == 3 && std::fabs(nestedHit->distance - 10.0f) < 1e-4f);
    if (nestedHit) CHECK(std::fabs(nestedHit->point.z - worldPoint.z) < 1e-4f);
    const render::Float3 localDirection{0.3f, -0.2f, -1.0f};
    const float localDirectionLength = std::sqrt(Dot(localDirection, localDirection));
    const render::Float3 unitLocalDirection{localDirection.x / localDirectionLength,
                                             localDirection.y / localDirectionLength,
                                             localDirection.z / localDirectionLength};
    constexpr float localRayDistance = 8.0f;
    const render::Float3 localOrigin{localPoint.x - unitLocalDirection.x * localRayDistance,
                                     localPoint.y - unitLocalDirection.y * localRayDistance,
                                     localPoint.z - unitLocalDirection.z * localRayDistance};
    const auto worldOrigin = ApplyPoint(localOrigin, world);
    const auto worldDirection = ApplyVector(unitLocalDirection, world);
    const float worldDirectionLength = std::sqrt(Dot(worldDirection, worldDirection));
    const render::Ray obliqueRay{worldOrigin, {worldDirection.x / worldDirectionLength,
                                               worldDirection.y / worldDirectionLength,
                                               worldDirection.z / worldDirectionLength}};
    const auto obliqueHit = render::Pick(obliqueRay, std::span<const render::PickTarget>(&nestedTarget, 1));
    CHECK(obliqueHit && obliqueHit->id == 3 &&
          std::fabs(obliqueHit->distance - localRayDistance * worldDirectionLength) < 1e-4f);
    if (obliqueHit)
        CHECK(std::fabs(obliqueHit->point.x - worldPoint.x) < 1e-4f &&
              std::fabs(obliqueHit->point.y - worldPoint.y) < 1e-4f &&
              std::fabs(obliqueHit->point.z - worldPoint.z) < 1e-4f);
    const auto outsidePoint = ApplyPoint({1.2f, 0, 1.0f}, world);
    CHECK(!render::Pick({{outsidePoint.x, outsidePoint.y, outsidePoint.z + 10}, {0, 0, -1}},
                        std::span<const render::PickTarget>(&nestedTarget, 1)));
    render::Float4x4 singular = world;
    singular.m[0][0] = singular.m[0][1] = singular.m[0][2] = 0;
    const render::PickTarget singularTarget{4, &model, singular};
    CHECK(!render::Pick({{worldPoint.x, worldPoint.y, worldPoint.z + 10}, {0, 0, -1}},
                        std::span<const render::PickTarget>(&singularTarget, 1)));

    render::OrbitCamera cam;
    cam.target = {0, 0, 0};
    const render::Rect vp{0, 0, 800, 600};
    const auto center = cam.ScreenRay(400, 300, vp);
    const auto eye = cam.Eye();
    const float len = std::sqrt(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z);
    CHECK(std::fabs(center.dir.x + eye.x / len) < 1e-4f && std::fabs(center.dir.z + eye.z / len) < 1e-4f);

    render::OrbitCamera frameCamera;
    frameCamera.target = {2, 3, 4};
    frameCamera.distance = 12;
    CHECK(frameCamera.TryFrame({0, 0, 0}, {0, 0, 0}));  // Point bounds use the safe minimum distance.
    CHECK(frameCamera.target.x == 0 && frameCamera.target.y == 0 && frameCamera.target.z == 0);
    CHECK(frameCamera.distance == 0.5f);
    const auto framedTarget = frameCamera.target;
    const float framedDistance = frameCamera.distance;
    CHECK(!frameCamera.TryFrame({1, 0, 0}, {0, 0, 0}));
    CHECK(frameCamera.target.x == framedTarget.x && frameCamera.target.y == framedTarget.y &&
          frameCamera.target.z == framedTarget.z && frameCamera.distance == framedDistance);
    CHECK(!frameCamera.TryFrame({-1e20f, -1e20f, -1e20f}, {1e20f, 1e20f, 1e20f}));
    CHECK(frameCamera.target.x == framedTarget.x && frameCamera.target.y == framedTarget.y &&
          frameCamera.target.z == framedTarget.z && frameCamera.distance == framedDistance);
    CHECK(!frameCamera.TryFrame({std::numeric_limits<float>::quiet_NaN(), 0, 0}, {1, 1, 1}));
    CHECK(frameCamera.target.x == framedTarget.x && frameCamera.distance == framedDistance);
    const float distantPoint = 1e12f;
    CHECK(!frameCamera.TryFrame({distantPoint, 0, 0}, {distantPoint, 0, 0}));
    CHECK(frameCamera.target.x == framedTarget.x && frameCamera.distance == framedDistance);

    constexpr std::array<float, 3> focusMin{-1, -1, -1};
    constexpr std::array<float, 3> focusMax{1, 1, 1};
    const double focusRadius = 0.5 * std::hypot(2.0, 2.0, 2.0);
    render::OrbitCamera squareAspectCamera;
    render::OrbitCamera narrowAspectCamera;
    render::OrbitCamera wideAspectCamera;
    CHECK(squareAspectCamera.TryFrame(focusMin, focusMax, 1.0f));
    CHECK(narrowAspectCamera.TryFrame(focusMin, focusMax, 0.01f));
    CHECK(wideAspectCamera.TryFrame(focusMin, focusMax, 100000.0f));
    const double verticalHalfFov = static_cast<double>(narrowAspectCamera.fovY) * 0.5;
    const double narrowHorizontalHalfFov = std::atan(std::tan(verticalHalfFov) * 0.01);
    CHECK(narrowAspectCamera.distance > squareAspectCamera.distance);
    CHECK(static_cast<double>(narrowAspectCamera.distance) * std::sin(verticalHalfFov) >= focusRadius * 1.099);
    CHECK(static_cast<double>(narrowAspectCamera.distance) * std::sin(narrowHorizontalHalfFov) >= focusRadius * 1.099);
    CHECK(std::fabs(wideAspectCamera.distance - squareAspectCamera.distance) < 1e-4f);

    render::OrbitCamera extremeAspectCamera;
    const auto extremeTarget = extremeAspectCamera.target;
    const float extremeDistance = extremeAspectCamera.distance;
    CHECK(!extremeAspectCamera.TryFrame(focusMin, focusMax, 1e-6f));
    CHECK(extremeAspectCamera.target.x == extremeTarget.x && extremeAspectCamera.target.y == extremeTarget.y &&
          extremeAspectCamera.target.z == extremeTarget.z && extremeAspectCamera.distance == extremeDistance);
    CHECK(extremeAspectCamera.TryFrame({0, 0, 0}, {0, 0, 0}, 1e-6f));
    CHECK(extremeAspectCamera.distance == 0.5f);
    const auto pointTarget = extremeAspectCamera.target;
    const float pointDistance = extremeAspectCamera.distance;
    CHECK(!extremeAspectCamera.TryFrame(focusMin, focusMax,
                                        std::numeric_limits<float>::quiet_NaN()));
    CHECK(extremeAspectCamera.target.x == pointTarget.x && extremeAspectCamera.target.y == pointTarget.y &&
          extremeAspectCamera.target.z == pointTarget.z && extremeAspectCamera.distance == pointDistance);
}

void TestTransformGizmoMath() {
    render::OrbitCamera camera;
    camera.target = {0, 0, 0};
    camera.distance = 10;
    const render::Rect viewport{40, 20, 800, 600};
    const auto view = camera.View();
    const auto projection = camera.Projection(viewport.w / viewport.h);
    render::ScreenPoint center;
    CHECK(render::ProjectPoint(camera.target, view, projection, viewport, center));
    CHECK(std::fabs(center.x - (viewport.x + viewport.w * 0.5f)) < 1e-3f);
    CHECK(std::fabs(center.y - (viewport.y + viewport.h * 0.5f)) < 1e-3f);
    const auto eye = camera.Eye();
    const render::Float3 behind{eye.x + (eye.x - camera.target.x),
                                eye.y + (eye.y - camera.target.y),
                                eye.z + (eye.z - camera.target.z)};
    render::ScreenPoint unchangedScreen{11, 12, 13};
    CHECK(!render::ProjectPoint(behind, view, projection, viewport, unchangedScreen));
    CHECK(unchangedScreen.x == 11 && unchangedScreen.y == 12 && unchangedScreen.depth == 13);

    const std::array<render::ScreenSegment, 3> handles{{
        {{10, 20, 0}, {100, 20, 0}},
        {{20, 10, 0}, {20, 100, 0}},
        {{50, 50, 0}, {53, 50, 0}},
    }};
    CHECK(render::HitTestScreenSegments(50, 23, handles, 5, 12) == 0);
    CHECK(render::HitTestScreenSegments(23, 50, handles, 5, 12) == 1);
    CHECK(render::HitTestScreenSegments(52, 51, handles, 5, 12) == -1);  // end-on handle
    CHECK(render::HitTestScreenSegments(300, 300, handles, 5, 12) == -1);
    const std::array<render::ScreenSegment, 2> overlapping{{handles[0], handles[0]}};
    CHECK(render::HitTestScreenSegments(50, 20, overlapping, 5, 12) == 1);  // visually topmost handle wins ties

    render::OrbitCamera endOnCamera;
    endOnCamera.target = {0, 0, 0};
    endOnCamera.distance = 10;
    endOnCamera.yaw = 0;
    endOnCamera.pitch = 0;
    const auto endOnView = endOnCamera.View();
    const auto endOnProjection = endOnCamera.Projection(viewport.w / viewport.h);
    render::ScreenPoint endOnCenter, endOnEndpoint;
    CHECK(render::ProjectPoint({0, 0, 0}, endOnView, endOnProjection, viewport, endOnCenter));
    CHECK(render::ProjectPoint({5, 0, 0}, endOnView, endOnProjection, viewport, endOnEndpoint));
    const render::ScreenSegment cameraFacingAxis{endOnCenter, endOnEndpoint};
    CHECK(render::HitTestScreenSegments(endOnEndpoint.x, endOnEndpoint.y,
                                        std::span<const render::ScreenSegment>(&cameraFacingAxis, 1)) == -1);

    float startParameter = -99;
    const render::Ray startRay{{10, 2, 0}, {0, -1, 0}};
    CHECK(render::ClosestRayAxisParameter(startRay, {0, 0, 0}, {1, 0, 0}, startParameter));
    CHECK(std::fabs(startParameter - 10) < 1e-5f);
    float currentParameter = 0;
    CHECK(render::ClosestRayAxisParameter({{13, 2, 0}, {0, -1, 0}}, {0, 0, 0},
                                          {1, 0, 0}, currentParameter));
    CHECK(std::fabs(currentParameter - 13) < 1e-5f);
    render::Float3 translated{99, 99, 99};
    CHECK(render::AxisTranslationFromParameter({4, 5, 6}, {1, 0, 0}, startParameter,
                                               currentParameter, translated));
    CHECK(std::fabs(translated.x - 7) < 1e-5f && translated.y == 5 && translated.z == 6);
    render::GizmoSnapSettings defaultSnapSettings;
    CHECK(!defaultSnapSettings.translationEnabled && !defaultSnapSettings.rotationEnabled &&
          !defaultSnapSettings.scaleEnabled && render::ValidateGizmoSnapSettings(defaultSnapSettings));
    auto invalidSnapSettings = defaultSnapSettings;
    invalidSnapSettings.translationStep = 0;
    CHECK(!render::ValidateGizmoSnapSettings(invalidSnapSettings));
    invalidSnapSettings = defaultSnapSettings;
    invalidSnapSettings.rotationStepDegrees = 181;
    CHECK(!render::ValidateGizmoSnapSettings(invalidSnapSettings));
    invalidSnapSettings = defaultSnapSettings;
    invalidSnapSettings.scaleStep = std::numeric_limits<float>::quiet_NaN();
    CHECK(!render::ValidateGizmoSnapSettings(invalidSnapSettings));

    float snappedDelta = 99;
    CHECK(render::SnapTranslationDelta(0.49f, 1.0f, snappedDelta) && snappedDelta == 0);
    CHECK(render::SnapTranslationDelta(0.51f, 1.0f, snappedDelta) && snappedDelta == 1);
    CHECK(render::SnapTranslationDelta(-0.51f, 1.0f, snappedDelta) && snappedDelta == -1);
    snappedDelta = 99;
    CHECK(!render::SnapTranslationDelta(1.0f, 0.0f, snappedDelta) && snappedDelta == 99);
    CHECK(!render::SnapTranslationDelta(std::numeric_limits<float>::infinity(), 1.0f, snappedDelta) &&
          snappedDelta == 99);
    constexpr float pi = 3.14159265358979323846f;
    CHECK(render::SnapRotationDelta(8.0f * pi / 180.0f, 15.0f, snappedDelta));
    CHECK(std::fabs(snappedDelta - 15.0f * pi / 180.0f) < 1e-6f);
    CHECK(render::SnapRotationDelta(-8.0f * pi / 180.0f, 15.0f, snappedDelta));
    CHECK(std::fabs(snappedDelta + 15.0f * pi / 180.0f) < 1e-6f);
    snappedDelta = 99;
    CHECK(!render::SnapRotationDelta(0.1f, 0.0f, snappedDelta) && snappedDelta == 99);
    CHECK(render::SnapScaleDelta(0.06f, 0.1f, snappedDelta) && std::fabs(snappedDelta - 0.1f) < 1e-6f);
    CHECK(render::SnapScaleDelta(-0.06f, 0.1f, snappedDelta) && std::fabs(snappedDelta + 0.1f) < 1e-6f);
    snappedDelta = 99;
    CHECK(!render::SnapScaleDelta(1.0f, 0.0f, snappedDelta) && snappedDelta == 99);
    CHECK(!render::SnapScaleDelta(1.0f, render::kMaxScaleSnapStep + 1.0f, snappedDelta) &&
          snappedDelta == 99);
    CHECK(render::AxisTranslationFromParameter({4, 5, 6}, {1, 0, 0}, 10, 10.51f,
                                               translated, 1.0f));
    CHECK(translated.x == 5 && translated.y == 5 && translated.z == 6);
    CHECK(render::AxisTranslationFromParameter({4, 5, 6}, {1, 0, 0}, 10, 9.49f,
                                               translated, 1.0f));
    CHECK(translated.x == 3 && translated.y == 5 && translated.z == 6);
    translated = {77, 77, 77};
    CHECK(!render::AxisTranslationFromParameter({4, 5, 6}, {1, 0, 0}, 10, 11,
                                                translated, -1.0f));
    CHECK(translated.x == 77 && translated.y == 77 && translated.z == 77);
    float unchangedParameter = 123;
    CHECK(!render::ClosestRayAxisParameter({{0, 1, 0}, {1, 0, 0}}, {0, 0, 0},
                                           {1, 0, 0}, unchangedParameter));
    CHECK(unchangedParameter == 123);

    const render::Float3 pointOnAxis{2.5f, 0, 0};
    render::ScreenPoint projectedAxisPoint;
    CHECK(render::ProjectPoint(pointOnAxis, view, projection, viewport, projectedAxisPoint));
    const auto axisRay = camera.ScreenRay(projectedAxisPoint.x, projectedAxisPoint.y, viewport);
    float recoveredAxisParameter = 0;
    CHECK(render::ClosestRayAxisParameter(axisRay, {0, 0, 0}, {1, 0, 0}, recoveredAxisParameter));
    CHECK(std::fabs(recoveredAxisParameter - pointOnAxis.x) < 1e-3f);

    float scaledAxisValue = -7;
    CHECK(render::AxisScaleFromParameter(1.25f, 3.0f, 93.0f, 90.0f, scaledAxisValue));
    CHECK(std::fabs(scaledAxisValue - 2.25f) < 1e-5f);
    CHECK(render::AxisScaleFromParameter(1.25f, 3.0f, -87.0f, 90.0f, scaledAxisValue));
    CHECK(std::fabs(scaledAxisValue - 0.25f) < 1e-5f);
    CHECK(render::AxisScaleFromParameter(1.25f, 0.0f, 0.26f, 1.0f, scaledAxisValue, 0.1f));
    CHECK(std::fabs(scaledAxisValue - 1.55f) < 1e-5f);
    scaledAxisValue = 17;
    CHECK(!render::AxisScaleFromParameter(1.25f, 0.0f, 0.26f, 1.0f, scaledAxisValue, -0.1f));
    CHECK(scaledAxisValue == 17);
    CHECK(render::AxisScaleFromParameter(0.1f, 0.0f, -0.1f, 1.0f, scaledAxisValue, 0.1f));
    CHECK(std::fabs(scaledAxisValue) < 1e-6f);
    scene::LocalTransform positiveScale;
    positiveScale.scale.x = 0.1f;
    const auto positiveScaleBefore = positiveScale;
    CHECK(!scene::SetLocalScaleAxis(positiveScale, 0, scaledAxisValue).ok());
    CHECK(positiveScale == positiveScaleBefore);
    CHECK(render::AxisScaleFromParameter(0.1f, 0.0f, -0.2f, 1.0f, scaledAxisValue, 0.1f));
    CHECK(scaledAxisValue < 0);
    CHECK(!scene::SetLocalScaleAxis(positiveScale, 0, scaledAxisValue).ok());
    CHECK(positiveScale == positiveScaleBefore);
    scaledAxisValue = 17;
    CHECK(!render::AxisScaleFromParameter(1.25f, 3.0f, 93.0f, 0.0f, scaledAxisValue));
    CHECK(scaledAxisValue == 17);
    render::Float4x4 collapsedFrame;
    for (auto& row : collapsedFrame.m)
        for (float& value : row) value = 0;
    render::Float3 unchangedDirection{7, 8, 9};
    CHECK(!render::NormalizeTransformedDirection({1, 0, 0}, collapsedFrame, unchangedDirection));
    CHECK(unchangedDirection.x == 7 && unchangedDirection.y == 8 && unchangedDirection.z == 9);

    const render::Float3 planeOrigin{1, -2, 0.5f};
    const render::Float3 planeU{2, 0, 0};
    const render::Float3 planeV{1, 1, 0};  // Affine image of a local ring under a nonuniform parent.
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    const float startU = static_cast<float>(std::cos(179.0 * kRadians));
    const float startV = static_cast<float>(std::sin(179.0 * kRadians));
    const render::Float3 ringPoint{planeOrigin.x + startU * planeU.x + startV * planeV.x,
                                   planeOrigin.y + startU * planeU.y + startV * planeV.y,
                                   planeOrigin.z + startU * planeU.z + startV * planeV.z};
    render::Float3 ringIntersection{88, 88, 88};
    CHECK(render::IntersectRayPlane({{ringPoint.x, ringPoint.y, ringPoint.z + 4}, {0, 0, -1}},
                                    planeOrigin, planeU, planeV, ringIntersection));
    CHECK(std::fabs(ringIntersection.x - ringPoint.x) < 1e-5f &&
          std::fabs(ringIntersection.y - ringPoint.y) < 1e-5f &&
          std::fabs(ringIntersection.z - ringPoint.z) < 1e-5f);
    float ringCoordinateU = -8, ringCoordinateV = -9;
    CHECK(render::PlaneBasisCoordinates(planeOrigin, planeU, planeV, ringIntersection,
                                       ringCoordinateU, ringCoordinateV));
    CHECK(std::fabs(ringCoordinateU - startU) < 1e-5f && std::fabs(ringCoordinateV - startV) < 1e-5f);
    float angleDelta = 7;
    const float endU = static_cast<float>(std::cos(-179.0 * kRadians));
    const float endV = static_cast<float>(std::sin(-179.0 * kRadians));
    CHECK(render::SignedPlanarAngleDelta(ringCoordinateU, ringCoordinateV, endU, endV, angleDelta));
    CHECK(std::fabs(angleDelta - static_cast<float>(2.0 * kRadians)) < 1e-5f);
    CHECK(render::SignedPlanarAngleDelta(endU, endV, ringCoordinateU, ringCoordinateV, angleDelta));
    CHECK(std::fabs(angleDelta + static_cast<float>(2.0 * kRadians)) < 1e-5f);
    angleDelta = 7;
    CHECK(!render::SignedPlanarAngleDelta(0, 0, 1, 0, angleDelta) && angleDelta == 7);
    ringIntersection = {88, 88, 88};
    CHECK(!render::IntersectRayPlane({{0, 0, 3}, {1, 0, 0}}, planeOrigin, planeU, planeV,
                                     ringIntersection));
    CHECK(ringIntersection.x == 88 && ringIntersection.y == 88 && ringIntersection.z == 88);
    CHECK(!render::IntersectRayPlane({{ringPoint.x, ringPoint.y, ringPoint.z - 4}, {0, 0, -1}},
                                     planeOrigin, planeU, planeV, ringIntersection));
    CHECK(ringIntersection.x == 88 && ringIntersection.y == 88 && ringIntersection.z == 88);
    ringCoordinateU = -8;
    ringCoordinateV = -9;
    CHECK(!render::PlaneBasisCoordinates(planeOrigin, planeU, {4, 0, 0}, ringPoint,
                                         ringCoordinateU, ringCoordinateV));
    CHECK(ringCoordinateU == -8 && ringCoordinateV == -9);

    std::array<render::ScreenSegment, render::kRotationRingSegments> projectedRing{};
    CHECK(render::ProjectRotationRing({0, 0, 0}, planeU, planeV, 0.75f,
                                     view, projection, viewport, projectedRing));
    const auto& ringHandle = projectedRing[0];
    const float ringPickX = (ringHandle.start.x + ringHandle.end.x) * 0.5f;
    const float ringPickY = (ringHandle.start.y + ringHandle.end.y) * 0.5f;
    CHECK(render::HitTestScreenSegments(ringPickX, ringPickY, projectedRing, 9.0f, 1.5f) >= 0);
    auto unchangedRing = projectedRing;
    CHECK(!render::ProjectRotationRing({0, 0, 0}, planeU, {4, 0, 0}, 0.75f,
                                       view, projection, viewport, projectedRing));
    for (size_t i = 0; i < projectedRing.size(); ++i)
        CHECK(projectedRing[i].start.x == unchangedRing[i].start.x &&
              projectedRing[i].start.y == unchangedRing[i].start.y &&
              projectedRing[i].end.x == unchangedRing[i].end.x &&
              projectedRing[i].end.y == unchangedRing[i].end.y);
}

scene::Document SceneFixture() {
    scene::Document document;
    document.id = "hangar-test";
    document.name = "Synthetic Hangar";
    scene::Node root;
    root.id = "root";
    root.name = "Hangar";
    root.kind = scene::NodeKind::Group;

    scene::Node bay;
    bay.id = "bay-a";
    bay.name = "Bay A";
    bay.kind = scene::NodeKind::Group;
    bay.parentId = "root";
    bay.localTransform.translation = {10.0f, -2.5f, 0.25f};
    bay.localTransform.rotation = {0, 0, 0.70710678f, 0.70710678f};
    bay.localTransform.scale = {2, 1, 1};

    scene::Node ship;
    ship.id = "ship-01";
    ship.name = "Test ship";
    ship.kind = scene::NodeKind::ModelInstance;
    ship.parentId = "bay-a";
    ship.assetPath = "content/models/test_ship.glb";
    ship.localTransform.translation = {1.0f, 2.0f, 3.0f};
    ship.localTransform.rotation = {0.0f, 0.0f, 0.38268343f, 0.92387950f};
    ship.localTransform.scale = {1.25f, 0.75f, 2.0f};

    document.nodes = {root, bay, ship};
    return document;
}

void TestSceneDocument(const fs::path& root) {
    auto document = SceneFixture();
    CHECK(scene::Validate(document).ok());
    const std::array<scene::ModelLocalBounds, 1> loadedShipBounds{{
        {"ship-01", {-1, -1, -1}, {1, 1, 1}},
    }};
    scene::ResolvedSubtreeBounds framedScene;
    CHECK(scene::ComputeSubtreeBounds(document, "root", loadedShipBounds, framedScene).ok());
    CHECK(framedScene.hasBounds() && framedScene.loadedModelCount == 1 &&
          framedScene.unavailableModelCount == 0);
    std::vector<scene::ResolvedTransform> shipWorldTransforms;
    CHECK(scene::ResolveWorldTransforms(document, shipWorldTransforms).ok());
    const auto shipWorld = std::find_if(shipWorldTransforms.begin(), shipWorldTransforms.end(),
        [](const auto& transform) { return transform.nodeId == "ship-01"; });
    CHECK(shipWorld != shipWorldTransforms.end());
    if (shipWorld != shipWorldTransforms.end()) {
        std::array<float, 3> expectedWorldMin{}, expectedWorldMax{};
        CHECK(render::TransformBounds({-1, -1, -1}, {1, 1, 1}, shipWorld->world,
                                      expectedWorldMin, expectedWorldMax));
        CHECK(framedScene.boundsMin == expectedWorldMin && framedScene.boundsMax == expectedWorldMax);
    }
    scene::ResolvedSubtreeBounds focusedShip;
    CHECK(scene::ComputeSubtreeBounds(document, "ship-01", loadedShipBounds, focusedShip).ok());
    CHECK(focusedShip.hasBounds() && focusedShip.loadedModelCount == 1);
    scene::ResolvedSubtreeBounds missingShip;
    CHECK(scene::ComputeSubtreeBounds(document, "ship-01", {}, missingShip).ok());
    CHECK(!missingShip.hasBounds() && missingShip.unavailableModelCount == 1);

    auto multipleModels = document;
    CHECK(scene::AddModelInstance(multipleModels, "ship-02", "Second ship", "content/models/second.obj",
                                   std::string("bay-a")).ok());
    CHECK(scene::AddGroup(multipleModels, "empty-group", "Empty group").ok());
    scene::ResolvedSubtreeBounds partialGroup;
    CHECK(scene::ComputeSubtreeBounds(multipleModels, "root", loadedShipBounds, partialGroup).ok());
    CHECK(partialGroup.hasBounds() && partialGroup.loadedModelCount == 1 &&
          partialGroup.unavailableModelCount == 1);
    scene::ResolvedSubtreeBounds emptyGroup;
    CHECK(scene::ComputeSubtreeBounds(multipleModels, "empty-group", loadedShipBounds, emptyGroup).ok());
    CHECK(!emptyGroup.hasBounds() && emptyGroup.loadedModelCount == 0 &&
          emptyGroup.unavailableModelCount == 0);
    scene::ResolvedSubtreeBounds noLoadedGroup;
    CHECK(scene::ComputeSubtreeBounds(multipleModels, "root", {}, noLoadedGroup).ok());
    CHECK(!noLoadedGroup.hasBounds() && noLoadedGroup.unavailableModelCount == 2);

    const auto framedSceneSentinel = framedScene;
    const std::array<scene::ModelLocalBounds, 1> invalidShipBounds{{
        {"ship-01", {1, 0, 0}, {-1, 0, 0}},
    }};
    CHECK(!scene::ComputeSubtreeBounds(document, "root", invalidShipBounds, framedScene).ok());
    CHECK(framedScene.loadedModelCount == framedSceneSentinel.loadedModelCount &&
          framedScene.unavailableModelCount == framedSceneSentinel.unavailableModelCount &&
          framedScene.boundsMin == framedSceneSentinel.boundsMin &&
          framedScene.boundsMax == framedSceneSentinel.boundsMax);
    CHECK(!scene::ComputeSubtreeBounds(document, "missing-node", loadedShipBounds, framedScene).ok());
    CHECK(framedScene.loadedModelCount == framedSceneSentinel.loadedModelCount &&
          framedScene.boundsMin == framedSceneSentinel.boundsMin &&
          framedScene.boundsMax == framedSceneSentinel.boundsMax);

    const std::array<scene::ModelLocalBounds, 1> pointShipBounds{{
        {"ship-01", {0, 0, 0}, {0, 0, 0}},
    }};
    scene::ResolvedSubtreeBounds pointBounds;
    CHECK(scene::ComputeSubtreeBounds(document, "ship-01", pointShipBounds, pointBounds).ok());
    CHECK(pointBounds.hasBounds() && pointBounds.boundsMin == pointBounds.boundsMax);
    CHECK(document == SceneFixture());
    scene::Document copiedDocument;
    CHECK(scene::MakeSceneCopy(document, "hangar-copy", copiedDocument).ok());
    auto expectedCopy = document;
    expectedCopy.id = "hangar-copy";
    CHECK(copiedDocument == expectedCopy);
    CHECK(document == SceneFixture());
    scene::Document unchangedCopyOutput = SceneFixture();
    const auto copyOutputSentinel = unchangedCopyOutput;
    CHECK(!scene::MakeSceneCopy(document, document.id, unchangedCopyOutput).ok());
    CHECK(unchangedCopyOutput == copyOutputSentinel);
    CHECK(!scene::MakeSceneCopy(document, "../outside", unchangedCopyOutput).ok());
    CHECK(unchangedCopyOutput == copyOutputSentinel);
    auto invalidCopySource = document;
    invalidCopySource.nodes[2].parentId = "missing-parent";
    CHECK(!scene::MakeSceneCopy(invalidCopySource, "invalid-copy", unchangedCopyOutput).ok());
    CHECK(unchangedCopyOutput == copyOutputSentinel);
    for (const scene::Vec3 euler : {scene::Vec3{31.0f, -47.0f, 122.0f},
                                   scene::Vec3{12.0f, 90.0f, 40.0f},
                                   scene::Vec3{-75.0f, -90.0f, 179.0f}}) {
        scene::Quaternion rotation;
        scene::Vec3 roundTripEuler;
        scene::Quaternion roundTripRotation;
        CHECK(scene::EulerXYZDegreesToQuaternion(euler, rotation).ok());
        CHECK(scene::QuaternionToEulerXYZDegrees(rotation, roundTripEuler).ok());
        CHECK(scene::EulerXYZDegreesToQuaternion(roundTripEuler, roundTripRotation).ok());
        render::Float4x4 matrix, roundTripMatrix;
        CHECK(render::AffineFromTrs({}, {rotation.x, rotation.y, rotation.z, rotation.w}, {1, 1, 1}, matrix));
        CHECK(render::AffineFromTrs({}, {roundTripRotation.x, roundTripRotation.y,
                                         roundTripRotation.z, roundTripRotation.w}, {1, 1, 1}, roundTripMatrix));
        for (size_t row = 0; row < 3; ++row)
            for (size_t column = 0; column < 3; ++column)
                CHECK(std::fabs(matrix.m[row][column] - roundTripMatrix.m[row][column]) < 2e-5f);
    }
    scene::Quaternion unchangedRotation{0.1f, 0.2f, 0.3f, 0.9f};
    const auto rotationSentinel = unchangedRotation;
    CHECK(!scene::EulerXYZDegreesToQuaternion({std::numeric_limits<float>::quiet_NaN(), 0, 0},
                                               unchangedRotation).ok());
    CHECK(unchangedRotation == rotationSentinel);
    scene::Vec3 unchangedEuler{1, 2, 3};
    CHECK(!scene::QuaternionToEulerXYZDegrees({0, 0, 0, 0}, unchangedEuler).ok());
    CHECK(unchangedEuler == (scene::Vec3{1, 2, 3}));

    constexpr float kHalfPi = 1.57079632679489661923f;
    scene::Quaternion quarterTurnAroundZ, localXTurn;
    CHECK(scene::EulerXYZDegreesToQuaternion({0, 0, 90}, quarterTurnAroundZ).ok());
    CHECK(scene::RotateLocalAxis(quarterTurnAroundZ, 0, kHalfPi, localXTurn).ok());
    render::Float4x4 startLocalRotation, changedLocalRotation;
    CHECK(render::AffineFromTrs({}, {quarterTurnAroundZ.x, quarterTurnAroundZ.y,
                                     quarterTurnAroundZ.z, quarterTurnAroundZ.w},
                                {1, 1, 1}, startLocalRotation));
    CHECK(render::AffineFromTrs({}, {localXTurn.x, localXTurn.y, localXTurn.z, localXTurn.w},
                                {1, 1, 1}, changedLocalRotation));
    render::Float3 startLocalXAxis, changedLocalXAxis, changedLocalYAxis;
    CHECK(render::TransformDirection({1, 0, 0}, startLocalRotation, startLocalXAxis));
    CHECK(render::TransformDirection({1, 0, 0}, changedLocalRotation, changedLocalXAxis));
    CHECK(render::TransformDirection({0, 1, 0}, changedLocalRotation, changedLocalYAxis));
    CHECK(std::fabs(startLocalXAxis.x - changedLocalXAxis.x) < 1e-5f &&
          std::fabs(startLocalXAxis.y - changedLocalXAxis.y) < 1e-5f &&
          std::fabs(startLocalXAxis.z - changedLocalXAxis.z) < 1e-5f);
    CHECK(std::fabs(changedLocalYAxis.x) < 1e-5f && std::fabs(changedLocalYAxis.y) < 1e-5f &&
          std::fabs(changedLocalYAxis.z - 1.0f) < 1e-5f);
    auto unchangedLocalRotation = quarterTurnAroundZ;
    CHECK(!scene::RotateLocalAxis(quarterTurnAroundZ, 3, kHalfPi, unchangedLocalRotation).ok());
    CHECK(unchangedLocalRotation == quarterTurnAroundZ);
    CHECK(!scene::RotateLocalAxis(quarterTurnAroundZ, 1,
                                  std::numeric_limits<float>::quiet_NaN(),
                                  unchangedLocalRotation).ok());
    CHECK(unchangedLocalRotation == quarterTurnAroundZ);

    auto scaledTransform = document.nodes[2].localTransform;
    const auto originalScale = scaledTransform.scale;
    CHECK(scene::ScaleUniform(scaledTransform, 1.1f).ok());
    CHECK(std::fabs(scaledTransform.scale.x - originalScale.x * 1.1f) < 1e-5f &&
          std::fabs(scaledTransform.scale.y - originalScale.y * 1.1f) < 1e-5f &&
          std::fabs(scaledTransform.scale.z - originalScale.z * 1.1f) < 1e-5f);
    CHECK(scene::ScaleUniform(scaledTransform, 1.0f / 1.1f).ok());
    CHECK(std::fabs(scaledTransform.scale.x - originalScale.x) < 1e-5f &&
          std::fabs(scaledTransform.scale.y - originalScale.y) < 1e-5f &&
          std::fabs(scaledTransform.scale.z - originalScale.z) < 1e-5f);
    const auto beforeRejectedScale = scaledTransform;
    CHECK(!scene::ScaleUniform(scaledTransform, 0).ok());
    CHECK(scaledTransform == beforeRejectedScale);
    auto maxScaleTransform = scaledTransform;
    maxScaleTransform.scale.x = 1000000.0f;
    const auto maxScaleBefore = maxScaleTransform;
    CHECK(!scene::ScaleUniform(maxScaleTransform, 2).ok());
    CHECK(maxScaleTransform == maxScaleBefore);

    auto axisScale = document.nodes[2].localTransform;
    const auto axisScaleOriginal = axisScale;
    CHECK(scene::SetLocalScaleAxis(axisScale, 1, 1.25f).ok());
    CHECK(axisScale.scale.x == axisScaleOriginal.scale.x && axisScale.scale.y == 1.25f &&
          axisScale.scale.z == axisScaleOriginal.scale.z);
    const auto axisScaleBeforeRefusal = axisScale;
    CHECK(!scene::SetLocalScaleAxis(axisScale, 0, -0.25f).ok());
    CHECK(axisScale == axisScaleBeforeRefusal);
    CHECK(!scene::SetLocalScaleAxis(axisScale, 0, 1000001.0f).ok());
    CHECK(axisScale == axisScaleBeforeRefusal);
    CHECK(!scene::SetLocalScaleAxis(axisScale, 0, std::numeric_limits<float>::quiet_NaN()).ok());
    CHECK(axisScale == axisScaleBeforeRefusal);
    CHECK(!scene::SetLocalScaleAxis(axisScale, 3, 2.0f).ok());
    CHECK(axisScale == axisScaleBeforeRefusal);
    auto invalidAxisScale = axisScaleBeforeRefusal;
    invalidAxisScale.scale.z = -1.0f;
    const auto invalidAxisScaleBefore = invalidAxisScale;
    CHECK(!scene::SetLocalScaleAxis(invalidAxisScale, 0, 2.0f).ok());
    CHECK(invalidAxisScale == invalidAxisScaleBefore);
    CHECK(scene::SetLocalScaleAxis(axisScale, 0, 0.000001f).ok());
    CHECK(scene::SetLocalScaleAxis(axisScale, 0, 1000000.0f).ok());

    const auto scaleDragBaseline = document;
    auto scaleDragDocument = scaleDragBaseline;
    const float scaleDragStart = scaleDragDocument.nodes[2].localTransform.scale.z;
    scene::DocumentHistory scaleDragHistory;
    bool scaleDragUndoCaptured = false;
    for (const float amount : {0.25f, 0.5f, 0.75f}) {
        auto candidate = scaleDragDocument;
        CHECK(scene::SetLocalScaleAxis(candidate.nodes[2].localTransform, 2,
                                       scaleDragStart + amount).ok());
        if (!scaleDragUndoCaptured) {
            CHECK(scaleDragHistory.RecordBefore(scaleDragDocument).ok());
            scaleDragUndoCaptured = true;
        }
        scaleDragDocument = std::move(candidate);
        std::vector<scene::ResolvedTransform> scaleDragWorld;
        CHECK(scene::ResolveWorldTransforms(scaleDragDocument, scaleDragWorld).ok());
    }
    CHECK(scaleDragHistory.retainedEntries() == 1);
    scene::Document scaleDragRestored;
    CHECK(scaleDragHistory.Undo(scaleDragDocument, scaleDragRestored).ok());
    CHECK(scaleDragRestored == scaleDragBaseline);
    scene::Document scaleDragRedone;
    CHECK(scaleDragHistory.Redo(scaleDragRestored, scaleDragRedone).ok());
    CHECK(scaleDragRedone == scaleDragDocument);

    const auto rotationDragBaseline = document;
    auto rotationDragDocument = rotationDragBaseline;
    const auto rotationDragStart = rotationDragDocument.nodes[2].localTransform.rotation;
    scene::DocumentHistory rotationDragHistory;
    bool rotationDragUndoCaptured = false;
    for (const float angle : {0.25f, 0.5f, 0.75f}) {
        auto candidate = rotationDragDocument;
        scene::Quaternion changedRotation;
        CHECK(scene::RotateLocalAxis(rotationDragStart, 1, angle, changedRotation).ok());
        candidate.nodes[2].localTransform.rotation = changedRotation;
        if (!rotationDragUndoCaptured) {
            CHECK(rotationDragHistory.RecordBefore(rotationDragDocument).ok());
            rotationDragUndoCaptured = true;
        }
        rotationDragDocument = std::move(candidate);
        std::vector<scene::ResolvedTransform> rotationDragWorld;
        CHECK(scene::ResolveWorldTransforms(rotationDragDocument, rotationDragWorld).ok());
    }
    CHECK(rotationDragHistory.retainedEntries() == 1);
    scene::Document rotationDragRestored;
    CHECK(rotationDragHistory.Undo(rotationDragDocument, rotationDragRestored).ok());
    CHECK(rotationDragRestored == rotationDragBaseline);
    scene::Document rotationDragRedone;
    CHECK(rotationDragHistory.Redo(rotationDragRestored, rotationDragRedone).ok());
    CHECK(rotationDragRedone == rotationDragDocument);

    scene::DocumentHistory rotationCancelHistory;
    CHECK(rotationCancelHistory.RecordBefore(rotationDragBaseline).ok());
    auto rotationBeforeGesture = rotationDragBaseline;
    rotationBeforeGesture.name = "Prior edit before rotation drag";
    CHECK(rotationCancelHistory.RecordBefore(rotationBeforeGesture).ok());
    auto rotationDuringGesture = rotationBeforeGesture;
    CHECK(scene::RotateLocalAxis(rotationBeforeGesture.nodes[2].localTransform.rotation, 2,
                                 0.8f, rotationDuringGesture.nodes[2].localTransform.rotation).ok());
    CHECK(rotationDuringGesture != rotationBeforeGesture);
    scene::Document rotationCanceled;
    CHECK(rotationCancelHistory.CancelLatestChange(rotationBeforeGesture, rotationCanceled).ok());
    CHECK(rotationCanceled == rotationBeforeGesture && rotationCancelHistory.retainedEntries() == 1);
    scene::Document rotationPriorUndo;
    CHECK(rotationCancelHistory.Undo(rotationCanceled, rotationPriorUndo).ok());
    CHECK(rotationPriorUndo == rotationDragBaseline);

    auto scaledHierarchy = document;
    std::vector<scene::ResolvedTransform> scaleWorldBefore;
    CHECK(scene::ResolveWorldTransforms(scaledHierarchy, scaleWorldBefore).ok());
    const auto scaleParent = std::find_if(scaleWorldBefore.begin(), scaleWorldBefore.end(), [](const auto& item) {
        return item.nodeId == "bay-a";
    });
    const auto scaleChild = std::find_if(scaleWorldBefore.begin(), scaleWorldBefore.end(), [](const auto& item) {
        return item.nodeId == "ship-01";
    });
    CHECK(scaleParent != scaleWorldBefore.end() && scaleChild != scaleWorldBefore.end());
    if (scaleParent != scaleWorldBefore.end() && scaleChild != scaleWorldBefore.end()) {
        render::Float4x4 childRotation;
        const auto& child = scaledHierarchy.nodes[2];
        CHECK(render::AffineFromTrs({}, {child.localTransform.rotation.x, child.localTransform.rotation.y,
                                         child.localTransform.rotation.z, child.localTransform.rotation.w},
                                    {1, 1, 1}, childRotation));
        const auto localScaleFrame = render::Multiply(childRotation, scaleParent->world);
        render::Float3 scaleXAxis, scaleYAxis;
        CHECK(render::NormalizeTransformedDirection({1, 0, 0}, localScaleFrame, scaleXAxis));
        CHECK(render::NormalizeTransformedDirection({0, 1, 0}, localScaleFrame, scaleYAxis));
        CHECK(std::fabs(std::sqrt(scaleXAxis.x * scaleXAxis.x + scaleXAxis.y * scaleXAxis.y +
                                  scaleXAxis.z * scaleXAxis.z) - 1.0f) < 1e-5f);
        const float axisDot = scaleXAxis.x * scaleYAxis.x + scaleXAxis.y * scaleYAxis.y +
                              scaleXAxis.z * scaleYAxis.z;
        CHECK(std::fabs(axisDot) > 0.1f);  // a rotated child under a nonuniform parent has sheared axes
        const auto scaleFrameOrigin = ApplyPoint({0, 0, 0}, localScaleFrame);
        const auto scaleFrameXAxis = ApplyPoint({1, 0, 0}, localScaleFrame);
        const render::Float3 rawScaleAxis{scaleFrameXAxis.x - scaleFrameOrigin.x,
                                          scaleFrameXAxis.y - scaleFrameOrigin.y,
                                          scaleFrameXAxis.z - scaleFrameOrigin.z};
        const auto beforePoint = ApplyPoint({2, 0, 0}, scaleChild->world);
        auto scaleNode = std::find_if(scaledHierarchy.nodes.begin(), scaledHierarchy.nodes.end(), [](const auto& item) {
            return item.id == "ship-01";
        });
        CHECK(scaleNode != scaledHierarchy.nodes.end());
        if (scaleNode != scaledHierarchy.nodes.end()) {
            const float originalLocalScale = scaleNode->localTransform.scale.x;
            CHECK(scene::SetLocalScaleAxis(scaleNode->localTransform, 0, originalLocalScale + 0.5f).ok());
            std::vector<scene::ResolvedTransform> scaleWorldAfter;
            CHECK(scene::ResolveWorldTransforms(scaledHierarchy, scaleWorldAfter).ok());
            const auto changedChild = std::find_if(scaleWorldAfter.begin(), scaleWorldAfter.end(), [](const auto& item) {
                return item.nodeId == "ship-01";
            });
            CHECK(changedChild != scaleWorldAfter.end());
            if (changedChild != scaleWorldAfter.end()) {
                const auto afterPoint = ApplyPoint({2, 0, 0}, changedChild->world);
                CHECK(std::fabs((afterPoint.x - beforePoint.x) - rawScaleAxis.x) < 1e-4f &&
                      std::fabs((afterPoint.y - beforePoint.y) - rawScaleAxis.y) < 1e-4f &&
                      std::fabs((afterPoint.z - beforePoint.z) - rawScaleAxis.z) < 1e-4f);
                CHECK(scaleNode->localTransform.scale.y == child.localTransform.scale.y &&
                      scaleNode->localTransform.scale.z == child.localTransform.scale.z);
            }
        }
    }
    auto rotatedHierarchy = document;
    std::vector<scene::ResolvedTransform> rotationWorldBefore;
    CHECK(scene::ResolveWorldTransforms(rotatedHierarchy, rotationWorldBefore).ok());
    const auto rotationParent = std::find_if(rotationWorldBefore.begin(), rotationWorldBefore.end(), [](const auto& item) {
        return item.nodeId == "bay-a";
    });
    const auto rotationChild = std::find_if(rotationWorldBefore.begin(), rotationWorldBefore.end(), [](const auto& item) {
        return item.nodeId == "ship-01";
    });
    CHECK(rotationParent != rotationWorldBefore.end() && rotationChild != rotationWorldBefore.end());
    auto rotationNode = std::find_if(rotatedHierarchy.nodes.begin(), rotatedHierarchy.nodes.end(), [](const auto& item) {
        return item.id == "ship-01";
    });
    CHECK(rotationNode != rotatedHierarchy.nodes.end());
    if (rotationParent != rotationWorldBefore.end() && rotationChild != rotationWorldBefore.end() &&
        rotationNode != rotatedHierarchy.nodes.end()) {
        const auto initialRotation = rotationNode->localTransform.rotation;
        render::Float4x4 initialLocalRotation;
        CHECK(render::AffineFromTrs({}, {initialRotation.x, initialRotation.y,
                                         initialRotation.z, initialRotation.w},
                                    {1, 1, 1}, initialLocalRotation));
        const auto initialAxisFrame = render::Multiply(initialLocalRotation, rotationParent->world);
        render::Float3 initialLocalXAxis;
        CHECK(render::TransformDirection({1, 0, 0}, initialAxisFrame, initialLocalXAxis));
        scene::Quaternion turnedRotation;
        CHECK(scene::RotateLocalAxis(initialRotation, 0, kHalfPi, turnedRotation).ok());
        rotationNode->localTransform.rotation = turnedRotation;
        std::vector<scene::ResolvedTransform> rotationWorldAfter;
        CHECK(scene::ResolveWorldTransforms(rotatedHierarchy, rotationWorldAfter).ok());
        const auto changedParent = std::find_if(rotationWorldAfter.begin(), rotationWorldAfter.end(), [](const auto& item) {
            return item.nodeId == "bay-a";
        });
        render::Float4x4 hierarchyChangedLocalRotation;
        CHECK(render::AffineFromTrs({}, {turnedRotation.x, turnedRotation.y, turnedRotation.z, turnedRotation.w},
                                    {1, 1, 1}, hierarchyChangedLocalRotation));
        if (changedParent != rotationWorldAfter.end()) {
            const auto changedAxisFrame = render::Multiply(hierarchyChangedLocalRotation, changedParent->world);
            render::Float3 hierarchyChangedLocalXAxis;
            CHECK(render::TransformDirection({1, 0, 0}, changedAxisFrame, hierarchyChangedLocalXAxis));
            CHECK(std::fabs(initialLocalXAxis.x - hierarchyChangedLocalXAxis.x) < 1e-4f &&
                  std::fabs(initialLocalXAxis.y - hierarchyChangedLocalXAxis.y) < 1e-4f &&
                  std::fabs(initialLocalXAxis.z - hierarchyChangedLocalXAxis.z) < 1e-4f);
        }
        CHECK(rotatedHierarchy.nodes[2].localTransform.scale == document.nodes[2].localTransform.scale);
        CHECK(scene::Validate(rotatedHierarchy).ok());
    }
    auto mirroredHierarchy = document;
    mirroredHierarchy.nodes[1].localTransform.scale.x = -2.0f;
    CHECK(!scene::Validate(mirroredHierarchy).ok());

    std::vector<scene::ResolvedTransform> resolved;
    CHECK(scene::ResolveWorldTransforms(document, resolved).ok());
    CHECK(resolved.size() == document.nodes.size());
    const auto parentWorld = std::find_if(resolved.begin(), resolved.end(), [](const auto& item) {
        return item.nodeId == "bay-a";
    });
    const auto childWorld = std::find_if(resolved.begin(), resolved.end(), [](const auto& item) {
        return item.nodeId == "ship-01";
    });
    CHECK(parentWorld != resolved.end() && childWorld != resolved.end());
    if (parentWorld != resolved.end() && childWorld != resolved.end()) {
        render::Float4x4 parentInverse;
        CHECK(render::InvertAffine(parentWorld->world, parentInverse));
        const auto startWorldOrigin = ApplyPoint({0, 0, 0}, childWorld->world);
        const render::Float3 worldY{0, 1, 0};
        const auto axisDocument = document;
        auto draggedDocument = axisDocument;
        scene::DocumentHistory gizmoHistory;
        bool undoCaptured = false;
        for (const float amount : {1.5f, 3.0f, 4.5f}) {
            render::Float3 targetWorld;
            CHECK(render::AxisTranslationFromParameter(startWorldOrigin, worldY, 0, amount, targetWorld));
            const auto targetLocal = ApplyPoint(targetWorld, parentInverse);
            auto candidate = draggedDocument;
            const auto node = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [](const auto& item) {
                return item.id == "ship-01";
            });
            CHECK(node != candidate.nodes.end());
            if (node == candidate.nodes.end()) continue;
            node->localTransform.translation = {targetLocal.x, targetLocal.y, targetLocal.z};
            if (!undoCaptured) {
                CHECK(gizmoHistory.RecordBefore(draggedDocument).ok());
                undoCaptured = true;
            }
            draggedDocument = std::move(candidate);
            std::vector<scene::ResolvedTransform> draggedWorld;
            CHECK(scene::ResolveWorldTransforms(draggedDocument, draggedWorld).ok());
            const auto draggedChild = std::find_if(draggedWorld.begin(), draggedWorld.end(), [](const auto& item) {
                return item.nodeId == "ship-01";
            });
            CHECK(draggedChild != draggedWorld.end());
            if (draggedChild != draggedWorld.end()) {
                const auto actual = ApplyPoint({0, 0, 0}, draggedChild->world);
                CHECK(std::fabs(actual.x - targetWorld.x) < 1e-4f &&
                      std::fabs(actual.y - targetWorld.y) < 1e-4f &&
                      std::fabs(actual.z - targetWorld.z) < 1e-4f);
            }
        }
        CHECK(gizmoHistory.retainedEntries() == 1);
        scene::Document restoredAxisDrag;
        CHECK(gizmoHistory.Undo(draggedDocument, restoredAxisDrag).ok());
        CHECK(restoredAxisDrag == axisDocument);
        scene::Document redoneAxisDrag;
        CHECK(gizmoHistory.Redo(restoredAxisDrag, redoneAxisDrag).ok());
        CHECK(redoneAxisDrag == draggedDocument);
    }
    scene::LocalTransform placedLocal;
    placedLocal.translation = {123, 456, 789};
    render::Float4x4 placedWorld;
    placedWorld.m[3][0] = -999;
    CHECK(resolved.size() > 1 && scene::ComputeGroundPlacement(resolved[1].world, {25, 7, 42},
                {-1, -1, -1}, {1, 1, 1}, placedLocal, placedWorld).ok());
    std::array<float, 3> placedMin{}, placedMax{};
    CHECK(render::TransformBounds({-1, -1, -1}, {1, 1, 1}, placedWorld, placedMin, placedMax));
    const auto placedOrigin = ApplyPoint({0, 0, 0}, placedWorld);
    CHECK(std::fabs(placedMin[2]) < 1e-4f && std::fabs(placedOrigin.x - 25) < 1e-4f &&
          std::fabs(placedOrigin.y - 7) < 1e-4f);
    const auto unchangedPlacement = placedLocal;
    const auto unchangedPlacementWorld = placedWorld;
    render::Float4x4 singularParent;
    singularParent.m[0][0] = 0;
    CHECK(!scene::ComputeGroundPlacement(singularParent, {1, 2, 0}, {-1, -1, -1}, {1, 1, 1},
                                         placedLocal, placedWorld).ok());
    CHECK(placedLocal == unchangedPlacement && placedWorld.m[3][0] == unchangedPlacementWorld.m[3][0]);
    if (resolved.size() == 3) {
        CHECK(resolved[0].nodeId == "root" && resolved[1].nodeId == "bay-a" && resolved[2].nodeId == "ship-01");
        const auto shipOrigin = ApplyPoint({0, 0, 0}, resolved[2].world);
        CHECK(std::fabs(shipOrigin.x - 8.0f) < 1e-4f && std::fabs(shipOrigin.y + 0.5f) < 1e-4f &&
              std::fabs(shipOrigin.z - 3.25f) < 1e-4f);
        auto reverseOrder = document;
        std::reverse(reverseOrder.nodes.begin(), reverseOrder.nodes.end());
        std::vector<scene::ResolvedTransform> reversedWorld;
        CHECK(scene::ResolveWorldTransforms(reverseOrder, reversedWorld).ok());
        CHECK(reversedWorld.size() == 3 && reversedWorld[0].nodeId == "ship-01");
        if (reversedWorld.size() == 3) {
            const auto reversedShipOrigin = ApplyPoint({0, 0, 0}, reversedWorld[0].world);
            CHECK(std::fabs(reversedShipOrigin.x - shipOrigin.x) < 1e-4f &&
                  std::fabs(reversedShipOrigin.y - shipOrigin.y) < 1e-4f &&
                  std::fabs(reversedShipOrigin.z - shipOrigin.z) < 1e-4f);
        }
    }

    std::string encoded;
    CHECK(scene::Serialize(document, encoded).ok());
    scene::Document decoded;
    CHECK(scene::Deserialize(encoded, decoded).ok());
    CHECK(decoded == document);

    // Dirty state follows the serialized saved baseline, including edits reverted to the exact
    // saved document. A bad update stays dirty but does not destroy the prior valid baseline.
    scene::DocumentDirtyState dirtyState;
    CHECK(dirtyState.SetBaseline(document, true).ok());
    CHECK(!dirtyState.dirty());
    auto editedDocument = document;
    editedDocument.nodes[2].name = "Edited test ship";
    CHECK(dirtyState.Update(editedDocument).ok());
    CHECK(dirtyState.dirty());
    CHECK(dirtyState.Update(document).ok());
    CHECK(!dirtyState.dirty());
    auto invalidDocument = editedDocument;
    invalidDocument.nodes[2].id = invalidDocument.nodes[1].id;
    CHECK(!dirtyState.Update(invalidDocument).ok());
    CHECK(dirtyState.dirty());
    CHECK(dirtyState.Update(document).ok());
    CHECK(!dirtyState.dirty());
    // A failed baseline establishment intentionally drops the old baseline, matching the editor's
    // behavior after a disk save followed by a serialization failure.
    CHECK(!dirtyState.SetBaseline(invalidDocument, true).ok());
    CHECK(dirtyState.dirty());
    CHECK(dirtyState.Update(document).ok());
    CHECK(dirtyState.dirty());
    CHECK(dirtyState.SetBaseline(document, false).ok());
    CHECK(dirtyState.dirty());

    scene::SceneActionGate actionGate;
    const scene::SceneAction openAction{scene::SceneActionKind::Open, "next-scene"};
    CHECK(actionGate.Request(openAction, false) == std::optional<scene::SceneAction>(openAction));
    CHECK(!actionGate.pending());
    CHECK(!actionGate.Request(openAction, true));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(openAction));
    CHECK(!actionGate.Resolve(scene::SceneActionChoice::Save, false));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(openAction));
    CHECK(actionGate.Resolve(scene::SceneActionChoice::Save, true) ==
          std::optional<scene::SceneAction>(openAction));
    CHECK(!actionGate.pending());
    const scene::SceneAction createAction{scene::SceneActionKind::Create, "new-scene"};
    CHECK(!actionGate.Request(createAction, true));
    CHECK(actionGate.Resolve(scene::SceneActionChoice::Discard) ==
          std::optional<scene::SceneAction>(createAction));
    const scene::SceneAction openProjectAction{
        scene::SceneActionKind::OpenProject, "C:/workspace/test-project"};
    CHECK(!actionGate.Request(openProjectAction, true));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(openProjectAction));
    CHECK(!actionGate.Resolve(scene::SceneActionChoice::Save, false));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(openProjectAction));
    CHECK(!actionGate.Resolve(scene::SceneActionChoice::Cancel));
    CHECK(!actionGate.pending());
    CHECK(!actionGate.Request(openProjectAction, true));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(openProjectAction));
    CHECK(actionGate.Resolve(scene::SceneActionChoice::Save, true) ==
          std::optional<scene::SceneAction>(openProjectAction));
    CHECK(!actionGate.pending());
    CHECK(!actionGate.Request(openProjectAction, true));
    CHECK(actionGate.Resolve(scene::SceneActionChoice::Discard) ==
          std::optional<scene::SceneAction>(openProjectAction));
    CHECK(!actionGate.pending());
    const scene::SceneAction createProjectAction{
        scene::SceneActionKind::CreateProject, "C:/workspace/new-project"};
    CHECK(!actionGate.Request(createProjectAction, true));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(createProjectAction));
    CHECK(!actionGate.Resolve(scene::SceneActionChoice::Save, false));
    CHECK(actionGate.pending() == std::optional<scene::SceneAction>(createProjectAction));
    CHECK(!actionGate.Resolve(scene::SceneActionChoice::Cancel));
    CHECK(!actionGate.pending());
    CHECK(!actionGate.Request(createProjectAction, true));
    CHECK(actionGate.Resolve(scene::SceneActionChoice::Discard) ==
          std::optional<scene::SceneAction>(createProjectAction));
    CHECK(!actionGate.pending());
    const scene::SceneAction quitAction{scene::SceneActionKind::Quit, {}};
    CHECK(!actionGate.Request(quitAction, true));
    CHECK(!actionGate.Resolve(scene::SceneActionChoice::Cancel));
    CHECK(!actionGate.pending());

    const fs::path projectRoot = root / "scene-project";
    const fs::path assetRoot = root / "scene-assets";
    fs::create_directories(projectRoot);
    fs::create_directories(assetRoot);
    WriteFile(assetRoot / "source.txt", "read-only asset bytes");
    Status openStatus;
    const std::array<fs::path, 1> readOnlyRoots{assetRoot};
    const std::array<fs::path, 1> mountedSources{assetRoot};

    const fs::path createParent = root / "project-creation-parent";
    fs::create_directories(createParent);
    const fs::path newProjectRoot = createParent / "fresh-project";
    CHECK(scene::ProjectStore::ValidateProjectCreateTarget(newProjectRoot, mountedSources,
                                                           readOnlyRoots).ok());
    Status createProjectStatus;
    auto createdProject = scene::ProjectStore::CreateProject(newProjectRoot, mountedSources,
                                                             readOnlyRoots, &createProjectStatus);
    CHECK(createdProject.has_value() && createProjectStatus.ok());
    CHECK(fs::is_directory(newProjectRoot));
    CHECK(fs::is_regular_file(newProjectRoot / "main.scene.json"));
    if (createdProject) {
        scene::Document createdMain;
        CHECK(createdProject->Load("main", createdMain, mountedSources).ok());
        CHECK(createdMain.id == "main" && createdMain.name == "Untitled" && createdMain.nodes.empty());
        std::vector<std::string> createdProjectScenes;
        CHECK(createdProject->ListScenes(createdProjectScenes, mountedSources).ok());
        CHECK(createdProjectScenes == std::vector<std::string>{"main"});
    }

    const fs::path existingProjectRoot = createParent / "existing-project";
    fs::create_directories(existingProjectRoot);
    WriteFile(existingProjectRoot / "keep.txt", "existing user data");
    CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(existingProjectRoot, mountedSources,
                                                            readOnlyRoots).ok());
    CHECK(!scene::ProjectStore::CreateProject(existingProjectRoot, mountedSources,
                                              readOnlyRoots, &createProjectStatus).has_value());
    std::ifstream existingProjectFile(existingProjectRoot / "keep.txt", std::ios::binary);
    std::string existingProjectBytes((std::istreambuf_iterator<char>(existingProjectFile)),
                                     std::istreambuf_iterator<char>());
    CHECK(existingProjectBytes == "existing user data");
    CHECK(!fs::exists(existingProjectRoot / "main.scene.json"));

    const fs::path existingProjectFilePath = createParent / "existing-project-file";
    WriteFile(existingProjectFilePath, "keep this file");
    CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(existingProjectFilePath, mountedSources,
                                                            readOnlyRoots).ok());
    CHECK(!scene::ProjectStore::CreateProject(existingProjectFilePath, mountedSources,
                                              readOnlyRoots, &createProjectStatus).has_value());
    std::ifstream existingProjectFileInput(existingProjectFilePath, std::ios::binary);
    std::string existingProjectFileBytes((std::istreambuf_iterator<char>(existingProjectFileInput)),
                                         std::istreambuf_iterator<char>());
    CHECK(existingProjectFileBytes == "keep this file");
    CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(fs::path{}, mountedSources,
                                                            readOnlyRoots).ok());

    // A path that was available during UI preflight may be claimed before the user confirms. The
    // create operation repeats the checks and leaves the newly-created contents alone.
    const fs::path racedProjectRoot = createParent / "claimed-after-preflight";
    CHECK(scene::ProjectStore::ValidateProjectCreateTarget(racedProjectRoot, mountedSources,
                                                           readOnlyRoots).ok());
    fs::create_directories(racedProjectRoot);
    WriteFile(racedProjectRoot / "keep.txt", "claimed by another process");
    CHECK(!scene::ProjectStore::CreateProject(racedProjectRoot, mountedSources,
                                              readOnlyRoots, &createProjectStatus).has_value());
    std::ifstream racedProjectFile(racedProjectRoot / "keep.txt", std::ios::binary);
    std::string racedProjectBytes((std::istreambuf_iterator<char>(racedProjectFile)),
                                  std::istreambuf_iterator<char>());
    CHECK(racedProjectBytes == "claimed by another process");
    CHECK(!fs::exists(racedProjectRoot / "main.scene.json"));

    const fs::path missingParentProject = root / "missing-project-parent" / "new-project";
    CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(missingParentProject, mountedSources,
                                                            readOnlyRoots).ok());
    CHECK(!scene::ProjectStore::CreateProject(missingParentProject, mountedSources,
                                              readOnlyRoots, &createProjectStatus).has_value());
    CHECK(!fs::exists(missingParentProject.parent_path()));

    const fs::path projectInsideAssets = assetRoot / "must-not-create-project";
    CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(projectInsideAssets, mountedSources,
                                                            readOnlyRoots).ok());
    CHECK(!scene::ProjectStore::CreateProject(projectInsideAssets, mountedSources,
                                              readOnlyRoots, &createProjectStatus).has_value());
    CHECK(!fs::exists(projectInsideAssets));

    std::error_code creationAliasError;
    const fs::path assetCreateAlias = root / "asset-root-create-alias";
    fs::create_directory_symlink(assetRoot, assetCreateAlias, creationAliasError);
    if (!creationAliasError) {
        const fs::path aliasProject = assetCreateAlias / "must-not-create-project";
        CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(aliasProject, mountedSources,
                                                                readOnlyRoots).ok());
        CHECK(!scene::ProjectStore::CreateProject(aliasProject, mountedSources,
                                                  readOnlyRoots, &createProjectStatus).has_value());
        CHECK(!fs::exists(assetRoot / "must-not-create-project"));
        fs::remove(assetCreateAlias, creationAliasError);
    }
    std::error_code targetSymlinkError;
    const fs::path existingProjectSymlink = createParent / "existing-project-symlink";
    fs::create_directory_symlink(assetRoot, existingProjectSymlink, targetSymlinkError);
    if (!targetSymlinkError) {
        CHECK(!scene::ProjectStore::ValidateProjectCreateTarget(existingProjectSymlink, mountedSources,
                                                                readOnlyRoots).ok());
        CHECK(!scene::ProjectStore::CreateProject(existingProjectSymlink, mountedSources,
                                                  readOnlyRoots, &createProjectStatus).has_value());
        CHECK(fs::is_regular_file(assetRoot / "source.txt"));
        fs::remove(existingProjectSymlink, targetSymlinkError);
    }
    for (const auto& entry : fs::directory_iterator(createParent))
        CHECK(!entry.path().filename().string().starts_with(".scuba-project-staging-"));

    auto project = scene::ProjectStore::Open(projectRoot, readOnlyRoots, &openStatus);
    CHECK(project && openStatus.ok());
    if (!project) return;
    CHECK(scene::ProjectStore::ValidateSourceSeparation(projectRoot, readOnlyRoots).ok());
    CHECK(!scene::ProjectStore::Open(assetRoot, readOnlyRoots).has_value());
    CHECK(!scene::ProjectStore::Open(assetRoot / "nested", readOnlyRoots).has_value());
    const fs::path notProjectDirectory = root / "not-a-project-directory";
    WriteFile(notProjectDirectory, "regular file, not a project directory");
    CHECK(!scene::ProjectStore::Open(notProjectDirectory, readOnlyRoots).has_value());
    const fs::path projectContainingAssets = root / "project-containing-assets";
    const fs::path nestedAssetRoot = projectContainingAssets / "mounted-assets";
    fs::create_directories(nestedAssetRoot);
    WriteFile(nestedAssetRoot / "source.txt", "nested read-only asset bytes");
    const std::array<fs::path, 1> nestedReadOnlyRoots{nestedAssetRoot};
    CHECK(!scene::ProjectStore::Open(projectContainingAssets, nestedReadOnlyRoots).has_value());
    CHECK(!fs::exists(projectContainingAssets / "main.scene.json"));
    std::ifstream nestedAssetInput(nestedAssetRoot / "source.txt", std::ios::binary);
    std::string nestedAssetBytes((std::istreambuf_iterator<char>(nestedAssetInput)),
                                 std::istreambuf_iterator<char>());
    CHECK(nestedAssetBytes == "nested read-only asset bytes");

    // Existing directory aliases resolve before overlap checks. Symlink creation may be disabled
    // by the host policy, so run these checks when the platform permits the fixture.
    std::error_code aliasError;
    const fs::path assetRootAlias = root / "scene-assets-alias";
    fs::create_directory_symlink(assetRoot, assetRootAlias, aliasError);
    if (!aliasError) {
        const std::array<fs::path, 1> assetAliasRoots{assetRootAlias};
        CHECK(scene::ProjectStore::ValidateSourceSeparation(projectRoot, assetAliasRoots).ok());
        CHECK(!scene::ProjectStore::ValidateSourceSeparation(assetRootAlias, readOnlyRoots).ok());
    }
    aliasError.clear();
    const fs::path projectRootAlias = root / "scene-project-alias";
    fs::create_directory_symlink(projectRoot, projectRootAlias, aliasError);
    if (!aliasError) {
        auto aliasedProject = scene::ProjectStore::Open(projectRootAlias, readOnlyRoots);
        CHECK(aliasedProject.has_value());
    }

#ifdef _WIN32
    // On a case-insensitive Windows directory, a case-only spelling of the same root must still
    // collide. Skip per-directory case-sensitive folders where the paths are distinct.
    std::string caseVariantName = projectRoot.filename().string();
    for (char& c : caseVariantName)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const fs::path caseVariantRoot = projectRoot.parent_path() / caseVariantName;
    std::error_code caseError;
    if (fs::equivalent(projectRoot, caseVariantRoot, caseError) && !caseError) {
        const std::array<fs::path, 1> caseVariantRoots{caseVariantRoot};
        CHECK(!scene::ProjectStore::ValidateSourceSeparation(projectRoot, caseVariantRoots).ok());
    }

    // If this temp parent permits distinct case-sensitive siblings, they are separate roots and
    // must not be rejected just because their spellings compare equal ignoring case.
    const fs::path caseDistinctLower = root / "case-distinct";
    const fs::path caseDistinctUpper = root / "CASE-DISTINCT";
    std::error_code distinctError;
    fs::create_directories(caseDistinctLower, distinctError);
    if (!distinctError) {
        distinctError.clear();
        fs::create_directories(caseDistinctUpper, distinctError);
        std::error_code equivalentError;
        if (!distinctError && fs::exists(caseDistinctUpper) &&
            !fs::equivalent(caseDistinctLower, caseDistinctUpper, equivalentError) && !equivalentError) {
            const std::array<fs::path, 1> separateCaseRoots{caseDistinctUpper};
            CHECK(scene::ProjectStore::ValidateSourceSeparation(caseDistinctLower, separateCaseRoots).ok());
        }
    }
#endif

    // An archive is a file source, but its location is still protected from project writes.
    const fs::path archiveProjectRoot = root / "archive-project";
    const fs::path archiveSource = archiveProjectRoot / "asset-source.zip";
    fs::create_directories(archiveProjectRoot);
    WriteFile(archiveSource, "archive fixture bytes");
    auto archiveProject = scene::ProjectStore::Open(archiveProjectRoot, std::span<const fs::path>{});
    CHECK(archiveProject.has_value());
    if (archiveProject) {
        scene::Document archiveScene;
        archiveScene.id = "archive-test";
        archiveScene.name = "Archive overlap test";
        const std::array<fs::path, 1> archiveSources{archiveSource};
        CHECK(!archiveProject->Save(archiveScene, archiveSources).ok());
        CHECK(!archiveProject->Create(archiveScene, archiveSources).ok());
        CHECK(!fs::exists(archiveProjectRoot / "archive-test.scene.json"));
        std::vector<std::string> archiveSceneList{"sentinel"};
        CHECK(!archiveProject->ListScenes(archiveSceneList, archiveSources).ok());
        CHECK(archiveSceneList == std::vector<std::string>{"sentinel"});
    }

    // Revalidate source paths at project-switch time and on later store operations. Here a mount
    // path is retargeted after project selection to a directory symlink into the project.
    const fs::path dynamicProjectRoot = root / "dynamic-project";
    const fs::path dynamicSourceRoot = root / "dynamic-source";
    const fs::path movedDynamicSource = root / "dynamic-source-original";
    fs::create_directories(dynamicProjectRoot);
    fs::create_directories(dynamicSourceRoot);
    const std::array<fs::path, 0> noMountedSources{};
    auto dynamicProject = scene::ProjectStore::Open(dynamicProjectRoot, noMountedSources);
    CHECK(dynamicProject.has_value());
    const std::array<fs::path, 1> dynamicSources{dynamicSourceRoot};
    scene::Document originalDynamicScene;
    originalDynamicScene.id = "main";
    originalDynamicScene.name = "Before mount path changed";
    if (dynamicProject) CHECK(dynamicProject->Save(originalDynamicScene, dynamicSources).ok());
    CHECK(scene::ProjectStore::ValidateSourceSeparation(dynamicProjectRoot, dynamicSources).ok());
    scene::SceneActionGate deferredProjectSwitch;
    const scene::SceneAction deferredOpenAction{
        scene::SceneActionKind::OpenProject, dynamicProjectRoot.string()};
    CHECK(!deferredProjectSwitch.Request(deferredOpenAction, true));

    std::error_code retargetError;
    fs::rename(dynamicSourceRoot, movedDynamicSource, retargetError);
    if (!retargetError) {
        retargetError.clear();
        fs::create_directory_symlink(dynamicProjectRoot, dynamicSourceRoot, retargetError);
    }
    if (!retargetError && dynamicProject) {
        const auto releasedAction = deferredProjectSwitch.Resolve(scene::SceneActionChoice::Discard);
        CHECK(releasedAction == std::optional<scene::SceneAction>(deferredOpenAction));
        CHECK(!scene::ProjectStore::ValidateSourceSeparation(dynamicProjectRoot, dynamicSources).ok());

        auto attemptedDynamicSave = originalDynamicScene;
        attemptedDynamicSave.name = "Must not overwrite after mount retarget";
        CHECK(!dynamicProject->Save(attemptedDynamicSave, dynamicSources).ok());
        scene::Document attemptedDynamicCreate;
        attemptedDynamicCreate.id = "dynamic-new";
        attemptedDynamicCreate.name = "Must not create inside retargeted mount";
        CHECK(!dynamicProject->Create(attemptedDynamicCreate, dynamicSources).ok());
        CHECK(!fs::exists(dynamicProjectRoot / "dynamic-new.scene.json"));
        scene::Document unchangedDynamicScene;
        unchangedDynamicScene.id = "sentinel";
        unchangedDynamicScene.name = "Must remain unchanged";
        const auto dynamicSceneSentinel = unchangedDynamicScene;
        CHECK(!dynamicProject->Load("main", unchangedDynamicScene, dynamicSources).ok());
        CHECK(unchangedDynamicScene == dynamicSceneSentinel);
        std::vector<std::string> unchangedSceneList{"sentinel"};
        CHECK(!dynamicProject->ListScenes(unchangedSceneList, dynamicSources).ok());
        CHECK(unchangedSceneList == std::vector<std::string>{"sentinel"});
        scene::Document storedDynamicScene;
        CHECK(dynamicProject->Load("main", storedDynamicScene).ok());
        CHECK(storedDynamicScene == originalDynamicScene);
    }
    std::error_code cleanupLinkError;
    if (fs::is_symlink(dynamicSourceRoot, cleanupLinkError)) fs::remove(dynamicSourceRoot, cleanupLinkError);
    std::error_code restoreSourceError;
    if (!fs::exists(dynamicSourceRoot) && fs::exists(movedDynamicSource))
        fs::rename(movedDynamicSource, dynamicSourceRoot, restoreSourceError);

    // Failed persistence cannot satisfy Save-and-continue. The pending action survives until a
    // valid save updates the baseline; only then is the transition released.
    CHECK(project->Save(document).ok());
    scene::DocumentDirtyState saveFlowDirty;
    CHECK(saveFlowDirty.SetBaseline(document, true).ok());
    auto saveFlowEdit = document;
    saveFlowEdit.name = "Save flow edit";
    CHECK(saveFlowDirty.Update(saveFlowEdit).ok() && saveFlowDirty.dirty());
    scene::SceneActionGate saveFlowGate;
    const scene::SceneAction saveFlowAction{scene::SceneActionKind::Open, "empty-scene"};
    CHECK(!saveFlowGate.Request(saveFlowAction, saveFlowDirty.dirty()));
    auto saveFlowInvalid = saveFlowEdit;
    saveFlowInvalid.nodes[2].id = saveFlowInvalid.nodes[1].id;
    CHECK(!project->Save(saveFlowInvalid).ok());
    CHECK(!saveFlowDirty.Update(saveFlowInvalid).ok() && saveFlowDirty.dirty());
    CHECK(!saveFlowGate.Resolve(scene::SceneActionChoice::Save, !saveFlowDirty.dirty()));
    CHECK(saveFlowGate.pending() == std::optional<scene::SceneAction>(saveFlowAction));
    scene::Document preservedAfterFailedSave;
    CHECK(project->Load("hangar-test", preservedAfterFailedSave).ok());
    CHECK(preservedAfterFailedSave == document);
    CHECK(project->Save(saveFlowEdit).ok());
    CHECK(saveFlowDirty.SetBaseline(saveFlowEdit, true).ok() && !saveFlowDirty.dirty());
    CHECK(saveFlowGate.Resolve(scene::SceneActionChoice::Save, !saveFlowDirty.dirty()) ==
          std::optional<scene::SceneAction>(saveFlowAction));

    CHECK(project->Save(document).ok());
    CHECK(fs::exists(projectRoot / "hangar-test.scene.json"));
    std::vector<std::string> projectScenes;
    CHECK(project->ListScenes(projectScenes).ok());
    CHECK(projectScenes == std::vector<std::string>{"hangar-test"});
    scene::Document emptyScene;
    emptyScene.id = "empty-scene";
    emptyScene.name = "Empty scene";
    CHECK(project->Create(emptyScene).ok());
    CHECK(project->ListScenes(projectScenes).ok());
    CHECK(projectScenes == (std::vector<std::string>{"empty-scene", "hangar-test"}));
    scene::Document copyToPersist;
    CHECK(scene::MakeSceneCopy(document, "hangar-copy", copyToPersist).ok());
    CHECK(project->Create(copyToPersist).ok());
    CHECK(project->ListScenes(projectScenes).ok());
    CHECK(projectScenes == (std::vector<std::string>{"empty-scene", "hangar-copy", "hangar-test"}));
    scene::Document persistedCopy;
    CHECK(project->Load("hangar-copy", persistedCopy).ok());
    CHECK(persistedCopy == copyToPersist);
    std::ifstream copiedSceneBeforeOverwrite(projectRoot / "hangar-copy.scene.json", std::ios::binary);
    const std::string copiedSceneBytesBeforeOverwrite(
        (std::istreambuf_iterator<char>(copiedSceneBeforeOverwrite)), std::istreambuf_iterator<char>());
    copiedSceneBeforeOverwrite.close();
    auto attemptedCopyOverwrite = copyToPersist;
    attemptedCopyOverwrite.name = "Must not replace existing copy";
    CHECK(!project->Create(attemptedCopyOverwrite).ok());
    std::ifstream copiedSceneAfterOverwrite(projectRoot / "hangar-copy.scene.json", std::ios::binary);
    const std::string copiedSceneBytesAfterOverwrite(
        (std::istreambuf_iterator<char>(copiedSceneAfterOverwrite)), std::istreambuf_iterator<char>());
    CHECK(copiedSceneBytesAfterOverwrite == copiedSceneBytesBeforeOverwrite);
    auto attemptedOverwrite = emptyScene;
    attemptedOverwrite.name = "Must not replace existing scene";
    CHECK(!project->Create(attemptedOverwrite).ok());
    scene::Document createdScene;
    CHECK(project->Load("empty-scene", createdScene).ok());
    CHECK(createdScene == emptyScene);
    std::ifstream assetInput(assetRoot / "source.txt", std::ios::binary);
    std::string assetBytes((std::istreambuf_iterator<char>(assetInput)), std::istreambuf_iterator<char>());
    CHECK(assetBytes == "read-only asset bytes");
    scene::Document reopened;
    CHECK(project->Load("hangar-test", reopened).ok());
    CHECK(reopened == document);
    CHECK(reopened.nodes.size() == 3);
    CHECK(reopened.nodes[1].parentId == std::optional<std::string>("root"));
    CHECK(reopened.nodes[2].parentId == std::optional<std::string>("bay-a"));
    CHECK(reopened.nodes[2].localTransform == document.nodes[2].localTransform);
    CHECK(reopened.nodes[2].assetPath == "content/models/test_ship.glb");

    // A second save atomically replaces the existing document.
    document.nodes[2].localTransform.translation.x = 9.5f;
    CHECK(project->Save(document).ok());
    CHECK(project->Load("hangar-test", reopened).ok());
    CHECK(reopened == document);

    // Failed validation cannot replace the last valid file.
    auto invalid = document;
    invalid.nodes[2].id = invalid.nodes[1].id;
    CHECK(!project->Save(invalid).ok());
    scene::Document preserved;
    CHECK(project->Load("hangar-test", preserved).ok());
    CHECK(preserved == document);

    // A malformed existing main scene fails without changing the caller's document, matching the
    // project-switch preflight behavior for malformed selected projects.
    WriteFile(projectRoot / "main.scene.json", "not a scene document");
    auto preserveOnLoadError = SceneFixture();
    const auto loadErrorSentinel = preserveOnLoadError;
    CHECK(!project->Load("main", preserveOnLoadError).ok());
    CHECK(preserveOnLoadError == loadErrorSentinel);

    auto duplicate = SceneFixture();
    duplicate.nodes[2].id = duplicate.nodes[1].id;
    CHECK(!scene::Validate(duplicate).ok());
    auto missingParent = SceneFixture();
    missingParent.nodes[1].parentId = "absent";
    CHECK(!scene::Validate(missingParent).ok());
    auto modelParent = SceneFixture();
    modelParent.nodes[1].parentId = "ship-01";
    CHECK(!scene::Validate(modelParent).ok());
    auto cycle = SceneFixture();
    cycle.nodes[0].parentId = "bay-a";
    CHECK(!scene::Validate(cycle).ok());
    auto invalidQuaternion = SceneFixture();
    invalidQuaternion.nodes[2].localTransform.rotation = {0, 0, 0, 0};
    CHECK(!scene::Validate(invalidQuaternion).ok());
    auto invalidUtf8 = SceneFixture();
    invalidUtf8.name = std::string("bad\xff", 4);
    CHECK(!scene::Validate(invalidUtf8).ok());
    auto invalidScale = SceneFixture();
    invalidScale.nodes[2].localTransform.scale.y = 0;
    CHECK(!scene::Validate(invalidScale).ok());

    scene::Document tooDeep;
    tooDeep.id = "too-deep";
    tooDeep.name = "Parent-first hierarchy over the limit";
    for (size_t i = 0; i <= scene::kMaxHierarchyDepth; ++i) {
        scene::Node group;
        group.id = "depth-" + std::to_string(i);
        group.name = "Group";
        group.kind = scene::NodeKind::Group;
        if (i) group.parentId = "depth-" + std::to_string(i - 1);
        tooDeep.nodes.push_back(std::move(group));
    }
    CHECK(!scene::Validate(tooDeep).ok());
    std::vector<scene::ResolvedTransform> unchangedDepth{{"sentinel", {}}};
    CHECK(!scene::ResolveWorldTransforms(tooDeep, unchangedDepth).ok());
    CHECK(unchangedDepth.size() == 1 && unchangedDepth.front().nodeId == "sentinel");
    tooDeep.nodes.pop_back();
    tooDeep.name = "Parent-first hierarchy at the limit";
    CHECK(scene::Validate(tooDeep).ok());

    scene::Document collapsed;
    collapsed.id = "collapsed";
    collapsed.name = "Collapsed hierarchy";
    for (size_t i = 0; i < 48; ++i) {
        scene::Node group;
        group.id = "g" + std::to_string(i);
        group.name = "Group";
        group.kind = scene::NodeKind::Group;
        if (i) group.parentId = "g" + std::to_string(i - 1);
        group.localTransform.scale = {1e-6f, 1e-6f, 1e-6f};
        collapsed.nodes.push_back(group);
    }
    scene::Node collapsedModel;
    collapsedModel.id = "model";
    collapsedModel.name = "Model";
    collapsedModel.kind = scene::NodeKind::ModelInstance;
    collapsedModel.parentId = "g47";
    collapsedModel.assetPath = "content/models/test_ship.glb";
    collapsed.nodes.push_back(collapsedModel);
    CHECK(scene::Validate(collapsed).ok());
    std::vector<scene::ResolvedTransform> unchanged{{"sentinel", {}}};
    CHECK(!scene::ResolveWorldTransforms(collapsed, unchanged).ok());
    CHECK(unchanged.size() == 1 && unchanged.front().nodeId == "sentinel");
    auto traversalAsset = SceneFixture();
    traversalAsset.nodes[2].assetPath = "../outside.obj";
    CHECK(!scene::Validate(traversalAsset).ok());
    auto traversalScene = SceneFixture();
    traversalScene.id = "../escape";
    CHECK(!project->Save(traversalScene).ok());
    CHECK(!project->Create(traversalScene).ok());
    traversalScene.id = "CON";
    CHECK(!project->Save(traversalScene).ok());
    traversalScene.id = "con";
    CHECK(!project->Save(traversalScene).ok());
    CHECK(!fs::exists(root / "escape.scene.json"));
    CHECK(!project->Load("../escape", reopened).ok());
    CHECK(reopened == preserved);

    scene::Document editable;
    editable.id = "editor-session";
    editable.name = "Editor session";
    CHECK(scene::AddGroup(editable, "group-root", "Group").ok());
    CHECK(scene::AddGroup(editable, "group-child", "Nested group", "group-root").ok());
    CHECK(scene::AddModelInstance(editable, "model-1", "Cube", "content/models/cube.glb", "group-child").ok());
    const scene::Document withPlacedModel = editable;
    CHECK(scene::AddModelInstance(editable, "model-1", "Duplicate", "content/models/other.glb").ok() == false);
    CHECK(editable == withPlacedModel);
    CHECK(scene::AddModelInstance(editable, "bad-path", "Invalid", "../outside.glb").ok() == false);
    CHECK(editable == withPlacedModel);
    CHECK(scene::Validate(editable).ok());
    CHECK(scene::RemoveSubtree(editable, "group-root").ok());
    CHECK(editable.nodes.empty());
    CHECK(scene::RemoveSubtree(editable, "missing").ok() == false);

    auto duplicateDocument = SceneFixture();
    const auto beforeDuplicate = duplicateDocument;
    const std::vector<std::string> duplicateIds{"copy-bay", "copy-ship"};
    size_t nextDuplicateId = 0;
    const auto duplicateIdGenerator = [&]() {
        if (nextDuplicateId >= duplicateIds.size()) return std::string{};
        return duplicateIds[nextDuplicateId++];
    };
    std::string duplicateRootId = "unchanged-output";
    CHECK(scene::DuplicateSubtree(duplicateDocument, "bay-a", duplicateIdGenerator,
                                  duplicateRootId).ok());
    CHECK(duplicateRootId == "copy-bay");
    CHECK(duplicateDocument.nodes.size() == beforeDuplicate.nodes.size() + 2);
    CHECK(duplicateDocument.nodes[0] == beforeDuplicate.nodes[0]);
    CHECK(duplicateDocument.nodes[1] == beforeDuplicate.nodes[1]);
    CHECK(duplicateDocument.nodes[2] == beforeDuplicate.nodes[2]);
    const auto copiedBay = std::find_if(duplicateDocument.nodes.begin(), duplicateDocument.nodes.end(),
                                        [](const auto& node) { return node.id == "copy-bay"; });
    const auto copiedShip = std::find_if(duplicateDocument.nodes.begin(), duplicateDocument.nodes.end(),
                                         [](const auto& node) { return node.id == "copy-ship"; });
    CHECK(copiedBay != duplicateDocument.nodes.end() && copiedBay->kind == scene::NodeKind::Group);
    CHECK(copiedShip != duplicateDocument.nodes.end() && copiedShip->kind == scene::NodeKind::ModelInstance);
    if (copiedBay != duplicateDocument.nodes.end()) {
        CHECK(copiedBay->name == "Bay A Copy");
        CHECK(copiedBay->parentId == std::optional<std::string>("root"));
        CHECK(copiedBay->localTransform.translation == scene::Vec3(11.0f, -2.5f, 0.25f));
    }
    if (copiedShip != duplicateDocument.nodes.end()) {
        CHECK(copiedShip->parentId == std::optional<std::string>("copy-bay"));
        CHECK(copiedShip->assetPath == beforeDuplicate.nodes[2].assetPath);
        CHECK(copiedShip->localTransform == beforeDuplicate.nodes[2].localTransform);
    }
    std::vector<scene::ResolvedTransform> duplicateWorlds;
    CHECK(scene::ResolveWorldTransforms(duplicateDocument, duplicateWorlds).ok());

    auto longNameDocument = SceneFixture();
    longNameDocument.nodes[2].name.assign(128, 'n');
    std::string longNameCopyId;
    CHECK(scene::DuplicateSubtree(longNameDocument, "ship-01",
                                  [] { return std::string("copy-long-name"); }, longNameCopyId).ok());
    const auto longNameCopy = std::find_if(longNameDocument.nodes.begin(), longNameDocument.nodes.end(),
        [&](const auto& node) { return node.id == longNameCopyId; });
    CHECK(longNameCopy != longNameDocument.nodes.end());
    if (longNameCopy != longNameDocument.nodes.end())
        CHECK(longNameCopy->name == std::string(128, 'n'));

    auto nestedDuplicateDocument = SceneFixture();
    std::vector<std::string> nestedDuplicateIds{"copy-root", "copy-nested-bay", "copy-nested-ship"};
    size_t nextNestedDuplicateId = 0;
    std::string nestedDuplicateRootId;
    CHECK(scene::DuplicateSubtree(nestedDuplicateDocument, "root", [&]() {
        if (nextNestedDuplicateId >= nestedDuplicateIds.size()) return std::string{};
        return nestedDuplicateIds[nextNestedDuplicateId++];
    }, nestedDuplicateRootId).ok());
    CHECK(nestedDuplicateRootId == "copy-root");
    const auto nestedBayCopy = std::find_if(nestedDuplicateDocument.nodes.begin(), nestedDuplicateDocument.nodes.end(),
        [](const auto& node) { return node.id == "copy-nested-bay"; });
    const auto nestedShipCopy = std::find_if(nestedDuplicateDocument.nodes.begin(), nestedDuplicateDocument.nodes.end(),
        [](const auto& node) { return node.id == "copy-nested-ship"; });
    CHECK(nestedBayCopy != nestedDuplicateDocument.nodes.end() &&
          nestedBayCopy->parentId == std::optional<std::string>("copy-root"));
    CHECK(nestedShipCopy != nestedDuplicateDocument.nodes.end() &&
          nestedShipCopy->parentId == std::optional<std::string>("copy-nested-bay"));

    auto reverseOrderedDuplicate = SceneFixture();
    std::reverse(reverseOrderedDuplicate.nodes.begin(), reverseOrderedDuplicate.nodes.end());
    CHECK(scene::Validate(reverseOrderedDuplicate).ok());
    std::vector<std::string> reverseOrderIds{"reverse-bay", "reverse-ship"};
    size_t nextReverseOrderId = 0;
    std::string reverseOrderRootId;
    CHECK(scene::DuplicateSubtree(reverseOrderedDuplicate, "bay-a", [&]() {
        if (nextReverseOrderId >= reverseOrderIds.size()) return std::string{};
        return reverseOrderIds[nextReverseOrderId++];
    }, reverseOrderRootId).ok());
    const auto reverseOrderShip = std::find_if(reverseOrderedDuplicate.nodes.begin(), reverseOrderedDuplicate.nodes.end(),
        [](const auto& node) { return node.id == "reverse-ship"; });
    CHECK(reverseOrderRootId == "reverse-bay" && reverseOrderShip != reverseOrderedDuplicate.nodes.end());
    if (reverseOrderShip != reverseOrderedDuplicate.nodes.end())
        CHECK(reverseOrderShip->parentId == std::optional<std::string>("reverse-bay"));

    auto translationLimitDocument = SceneFixture();
    translationLimitDocument.nodes[2].localTransform.translation.x = 10000000.0f;
    const auto translationLimitBefore = translationLimitDocument;
    std::string translationLimitOutput = "unchanged-output";
    size_t translationLimitGeneratorCalls = 0;
    CHECK(!scene::DuplicateSubtree(translationLimitDocument, "ship-01", [&]() {
        ++translationLimitGeneratorCalls;
        return std::string("copy-at-limit");
    }, translationLimitOutput).ok());
    CHECK(translationLimitGeneratorCalls == 1);
    CHECK(translationLimitDocument == translationLimitBefore);
    CHECK(translationLimitOutput == "unchanged-output");

    scene::DocumentHistory duplicateHistory;
    CHECK(duplicateHistory.RecordBefore(beforeDuplicate).ok());
    scene::Document duplicateUndo;
    CHECK(duplicateHistory.Undo(duplicateDocument, duplicateUndo).ok());
    CHECK(duplicateUndo == beforeDuplicate);
    scene::Document duplicateRedo;
    CHECK(duplicateHistory.Redo(duplicateUndo, duplicateRedo).ok());
    CHECK(duplicateRedo == duplicateDocument);
    scene::DocumentDirtyState duplicateDirty;
    CHECK(duplicateDirty.SetBaseline(beforeDuplicate, true).ok());
    CHECK(duplicateDirty.Update(duplicateDocument).ok() && duplicateDirty.dirty());
    CHECK(duplicateDirty.Update(duplicateUndo).ok() && !duplicateDirty.dirty());

    auto duplicateFailure = beforeDuplicate;
    std::string unchangedDuplicateRoot = "keep-this-output";
    size_t missingGeneratorCalls = 0;
    CHECK(!scene::DuplicateSubtree(duplicateFailure, "missing",
                                   [&]() { ++missingGeneratorCalls; return std::string("unused"); },
                                   unchangedDuplicateRoot).ok());
    CHECK(missingGeneratorCalls == 0 && duplicateFailure == beforeDuplicate);
    CHECK(unchangedDuplicateRoot == "keep-this-output");
    CHECK(!scene::DuplicateSubtree(duplicateFailure, "bay-a",
                                   [] { return std::string("root"); }, unchangedDuplicateRoot).ok());
    CHECK(duplicateFailure == beforeDuplicate && unchangedDuplicateRoot == "keep-this-output");
    size_t partialGeneratorCalls = 0;
    CHECK(!scene::DuplicateSubtree(duplicateFailure, "bay-a", [&]() {
        return partialGeneratorCalls++ == 0 ? std::string("copy-bay") : std::string{};
    }, unchangedDuplicateRoot).ok());
    CHECK(duplicateFailure == beforeDuplicate && unchangedDuplicateRoot == "keep-this-output");
    CHECK(!scene::DuplicateSubtree(duplicateFailure, "bay-a", duplicateIdGenerator,
                                   unchangedDuplicateRoot, {std::numeric_limits<float>::infinity(), 0, 0}).ok());
    CHECK(duplicateFailure == beforeDuplicate && unchangedDuplicateRoot == "keep-this-output");

    auto fullDocument = scene::Document{};
    fullDocument.id = "duplicate-limit";
    fullDocument.name = "Duplicate limit";
    scene::Node limitRoot;
    limitRoot.id = "limit-root";
    limitRoot.name = "Limit root";
    limitRoot.kind = scene::NodeKind::Group;
    scene::Node limitChild;
    limitChild.id = "limit-child";
    limitChild.name = "Limit child";
    limitChild.kind = scene::NodeKind::Group;
    limitChild.parentId = "limit-root";
    fullDocument.nodes = {limitRoot, limitChild};
    for (size_t i = 0; i < scene::kMaxSceneNodes - 3; ++i) {
        scene::Node filler;
        filler.id = "filler-" + std::to_string(i);
        filler.name = "Filler";
        filler.kind = scene::NodeKind::Group;
        fullDocument.nodes.push_back(std::move(filler));
    }
    CHECK(fullDocument.nodes.size() == scene::kMaxSceneNodes - 1);
    CHECK(scene::Validate(fullDocument).ok());
    const auto fullDocumentBeforeDuplicate = fullDocument;
    size_t limitGeneratorCalls = 0;
    CHECK(!scene::DuplicateSubtree(fullDocument, "limit-root", [&]() {
        ++limitGeneratorCalls;
        return std::string("limit-copy");
    }, unchangedDuplicateRoot).ok());
    CHECK(limitGeneratorCalls == 0 && fullDocument == fullDocumentBeforeDuplicate);
    CHECK(unchangedDuplicateRoot == "keep-this-output");

    auto renameDocument = SceneFixture();
    CHECK(scene::RenameNode(renameDocument, "bay-a", "Hangar Bay").ok());
    CHECK(renameDocument.nodes[1].name == "Hangar Bay");
    const auto renamedDocument = renameDocument;
    CHECK(!scene::RenameNode(renameDocument, "bay-a", "").ok());
    CHECK(renameDocument == renamedDocument);
    CHECK(!scene::RenameNode(renameDocument, "absent", "Missing").ok());

    auto reparentDocument = SceneFixture();
    std::vector<scene::ResolvedTransform> beforeReparent;
    CHECK(scene::ResolveWorldTransforms(reparentDocument, beforeReparent).ok());
    CHECK(scene::ReparentNode(reparentDocument, "bay-a", std::nullopt).ok());
    CHECK(!reparentDocument.nodes[1].parentId);
    std::vector<scene::ResolvedTransform> afterReparent;
    CHECK(scene::ResolveWorldTransforms(reparentDocument, afterReparent).ok());
    for (const auto& previous : beforeReparent) {
        const auto current = std::find_if(afterReparent.begin(), afterReparent.end(), [&](const auto& item) {
            return item.nodeId == previous.nodeId;
        });
        CHECK(current != afterReparent.end());
        if (current != afterReparent.end())
            for (size_t row = 0; row < 4; ++row)
                for (size_t column = 0; column < 4; ++column)
                    CHECK(std::fabs(previous.world.m[row][column] - current->world.m[row][column]) < 1e-3f);
    }
    const auto preservedReparent = reparentDocument;
    auto cycleDocument = SceneFixture();
    const auto unchangedCycle = cycleDocument;
    CHECK(!scene::ReparentNode(cycleDocument, "root", std::string("bay-a")).ok());  // descendant parent
    CHECK(cycleDocument == unchangedCycle);
    auto modelParentDocument = SceneFixture();
    const auto unchangedModelParent = modelParentDocument;
    CHECK(!scene::ReparentNode(modelParentDocument, "bay-a", std::string("ship-01")).ok());
    CHECK(modelParentDocument == unchangedModelParent);
    CHECK(!scene::ReparentNode(reparentDocument, "bay-a", std::string("missing")).ok());
    CHECK(reparentDocument == preservedReparent);

    auto shearedReparent = SceneFixture();
    const auto unchangedSheared = shearedReparent;
    CHECK(!scene::ReparentNode(shearedReparent, "ship-01", std::nullopt).ok());
    CHECK(shearedReparent == unchangedSheared);

    scene::Document rotatedReparent;
    rotatedReparent.id = "rotated-reparent";
    rotatedReparent.name = "Rotated reparent fixture";
    scene::Node rotatedParent;
    rotatedParent.id = "rotated-parent";
    rotatedParent.name = "Rotated parent";
    rotatedParent.kind = scene::NodeKind::Group;
    rotatedParent.localTransform.translation = {4, -2, 1};
    rotatedParent.localTransform.rotation = {0.18257418f, 0.36514837f, 0.54772256f, 0.73029674f};
    rotatedParent.localTransform.scale = {2, 2, 2};
    scene::Node rotatedChild;
    rotatedChild.id = "rotated-child";
    rotatedChild.name = "Rotated child";
    rotatedChild.kind = scene::NodeKind::Group;
    rotatedChild.parentId = "rotated-parent";
    rotatedChild.localTransform.translation = {-1, 3, 0.5f};
    rotatedChild.localTransform.rotation = {0.40824829f, 0.40824829f, 0.40824829f, 0.70710678f};
    rotatedChild.localTransform.scale = {0.5f, 1.5f, 2};
    scene::Node rotatedDestination;
    rotatedDestination.id = "rotated-destination";
    rotatedDestination.name = "Rotated destination";
    rotatedDestination.kind = scene::NodeKind::Group;
    rotatedDestination.localTransform.translation = {-6, 2, 3};
    rotatedDestination.localTransform.rotation = {0, 0.25881904f, 0, 0.96592583f};
    rotatedDestination.localTransform.scale = {0.75f, 0.75f, 0.75f};
    rotatedReparent.nodes = {rotatedParent, rotatedChild, rotatedDestination};
    std::vector<scene::ResolvedTransform> rotatedWorldBefore;
    CHECK(scene::ResolveWorldTransforms(rotatedReparent, rotatedWorldBefore).ok());
    CHECK(scene::ReparentNode(rotatedReparent, "rotated-child", std::string("rotated-destination")).ok());
    std::vector<scene::ResolvedTransform> rotatedWorldAfter;
    CHECK(scene::ResolveWorldTransforms(rotatedReparent, rotatedWorldAfter).ok());
    for (const auto& previous : rotatedWorldBefore) {
        const auto current = std::find_if(rotatedWorldAfter.begin(), rotatedWorldAfter.end(), [&](const auto& item) {
            return item.nodeId == previous.nodeId;
        });
        CHECK(current != rotatedWorldAfter.end());
        if (current != rotatedWorldAfter.end())
            for (size_t row = 0; row < 4; ++row)
                for (size_t column = 0; column < 4; ++column)
                    CHECK(std::fabs(previous.world.m[row][column] - current->world.m[row][column]) < 1e-3f);
    }

    auto historyReparent = SceneFixture();
    const auto historyReparentBase = historyReparent;
    scene::DocumentHistory reparentHistory;
    CHECK(reparentHistory.RecordBefore(historyReparent).ok());
    CHECK(scene::ReparentNode(historyReparent, "bay-a", std::nullopt).ok());
    scene::Document restoredReparent;
    CHECK(reparentHistory.Undo(historyReparent, restoredReparent).ok());
    CHECK(restoredReparent == historyReparentBase);

    scene::DocumentHistory history;
    const auto historyBase = SceneFixture();
    auto historyOne = historyBase;
    historyOne.nodes[2].localTransform.translation.x += 1.0f;
    auto historyTwo = historyOne;
    historyTwo.nodes[2].localTransform.translation.y += 2.0f;
    CHECK(history.RecordBefore(historyBase).ok());
    CHECK(history.RecordBefore(historyOne).ok());
    scene::Document historyRestored;
    CHECK(history.Undo(historyTwo, historyRestored).ok() && historyRestored == historyOne);
    CHECK(history.CanUndo() && history.CanRedo());
    CHECK(history.Undo(historyRestored, historyRestored).ok() && historyRestored == historyBase);
    CHECK(!history.CanUndo() && history.CanRedo());
    CHECK(history.Redo(historyRestored, historyRestored).ok() && historyRestored == historyOne);
    CHECK(history.CanRedo());
    auto historyBranch = historyOne;
    historyBranch.nodes[2].localTransform.translation.z += 3.0f;
    CHECK(history.RecordBefore(historyOne).ok());  // a new edit discards the old redo branch
    CHECK(!history.CanRedo());
    CHECK(history.Undo(historyBranch, historyRestored).ok() && historyRestored == historyOne);

    scene::DocumentHistory cancelHistory;
    CHECK(cancelHistory.RecordBefore(historyBase).ok());
    auto cancelIntermediate = historyBase;
    cancelIntermediate.nodes[2].localTransform.scale.y += 0.1f;
    CHECK(cancelHistory.RecordBefore(cancelIntermediate).ok());
    auto canceledDragDocument = cancelIntermediate;
    canceledDragDocument.nodes[2].localTransform.scale.x += 0.5f;
    CHECK(canceledDragDocument != cancelIntermediate);
    auto wrongCancelBaseline = historyBase;
    wrongCancelBaseline.name = "a different transaction";
    auto cancelOutputSentinel = historyTwo;
    CHECK(!cancelHistory.CancelLatestChange(wrongCancelBaseline, cancelOutputSentinel).ok());
    CHECK(cancelHistory.retainedEntries() == 2 && cancelOutputSentinel == historyTwo);
    scene::Document canceledDragRestored;
    CHECK(cancelHistory.CancelLatestChange(cancelIntermediate, canceledDragRestored).ok());
    CHECK(canceledDragRestored == cancelIntermediate);
    CHECK(cancelHistory.CanUndo() && !cancelHistory.CanRedo());
    CHECK(cancelHistory.retainedEntries() == 1);
    scene::DocumentDirtyState cancelDirtyState;
    CHECK(cancelDirtyState.SetBaseline(cancelIntermediate, true).ok());
    CHECK(cancelDirtyState.Update(canceledDragDocument).ok() && cancelDirtyState.dirty());
    CHECK(cancelDirtyState.Update(canceledDragRestored).ok() && !cancelDirtyState.dirty());
    scene::Document priorCancelRestored;
    CHECK(cancelHistory.Undo(canceledDragRestored, priorCancelRestored).ok());
    CHECK(priorCancelRestored == historyBase && cancelHistory.retainedEntries() == 1);
    scene::DocumentHistory redoCancelHistory;
    CHECK(redoCancelHistory.RecordBefore(historyBase).ok());
    CHECK(redoCancelHistory.RecordBefore(historyOne).ok());
    scene::Document redoCancelCurrent;
    CHECK(redoCancelHistory.Undo(historyTwo, redoCancelCurrent).ok());
    auto redoCancelSentinel = historyTwo;
    CHECK(!redoCancelHistory.CancelLatestChange(historyBase, redoCancelSentinel).ok());
    CHECK(redoCancelSentinel == historyTwo && redoCancelHistory.CanUndo() && redoCancelHistory.CanRedo());

    scene::DocumentHistory rotationScaleHistory;
    const auto rotationScaleBase = SceneFixture();
    auto rotationScaleEdited = rotationScaleBase;
    scene::Quaternion editedRotation;
    CHECK(scene::EulerXYZDegreesToQuaternion({25.0f, -18.0f, 73.0f}, editedRotation).ok());
    rotationScaleEdited.nodes[2].localTransform.rotation = editedRotation;
    CHECK(scene::ScaleUniform(rotationScaleEdited.nodes[2].localTransform, 1.2f).ok());
    std::vector<scene::ResolvedTransform> rotationScaleWorld;
    CHECK(scene::ResolveWorldTransforms(rotationScaleEdited, rotationScaleWorld).ok());
    CHECK(rotationScaleHistory.RecordBefore(rotationScaleBase).ok());
    scene::Document rotationScaleRestored;
    CHECK(rotationScaleHistory.Undo(rotationScaleEdited, rotationScaleRestored).ok());
    CHECK(rotationScaleRestored == rotationScaleBase);
    scene::Document rotationScaleRedone;
    CHECK(rotationScaleHistory.Redo(rotationScaleRestored, rotationScaleRedone).ok());
    CHECK(rotationScaleRedone == rotationScaleEdited);

    auto parentedWorldSnap = SceneFixture();
    std::vector<scene::ResolvedTransform> beforeWorldSnap;
    CHECK(scene::ResolveWorldTransforms(parentedWorldSnap, beforeWorldSnap).ok());
    const auto parentWorldBeforeSnap = std::find_if(beforeWorldSnap.begin(), beforeWorldSnap.end(),
        [](const auto& item) { return item.nodeId == "bay-a"; });
    const auto modelWorldBeforeSnap = std::find_if(beforeWorldSnap.begin(), beforeWorldSnap.end(),
        [](const auto& item) { return item.nodeId == "ship-01"; });
    CHECK(parentWorldBeforeSnap != beforeWorldSnap.end() && modelWorldBeforeSnap != beforeWorldSnap.end());
    if (parentWorldBeforeSnap != beforeWorldSnap.end() && modelWorldBeforeSnap != beforeWorldSnap.end()) {
        const render::Float3 startWorld{modelWorldBeforeSnap->world.m[3][0],
                                        modelWorldBeforeSnap->world.m[3][1],
                                        modelWorldBeforeSnap->world.m[3][2]};
        render::Float3 snappedWorld;
        CHECK(render::AxisTranslationFromParameter(startWorld, {0, 1, 0}, 0, 1.6f,
                                                   snappedWorld, 1.0f));
        render::Float4x4 parentInverse;
        CHECK(render::InvertAffine(parentWorldBeforeSnap->world, parentInverse));
        const auto applyPoint = [](render::Float3 point, const render::Float4x4& matrix) {
            return render::Float3{
                point.x * matrix.m[0][0] + point.y * matrix.m[1][0] + point.z * matrix.m[2][0] + matrix.m[3][0],
                point.x * matrix.m[0][1] + point.y * matrix.m[1][1] + point.z * matrix.m[2][1] + matrix.m[3][1],
                point.x * matrix.m[0][2] + point.y * matrix.m[1][2] + point.z * matrix.m[2][2] + matrix.m[3][2],
            };
        };
        auto model = std::find_if(parentedWorldSnap.nodes.begin(), parentedWorldSnap.nodes.end(),
                                  [](const auto& node) { return node.id == "ship-01"; });
        CHECK(model != parentedWorldSnap.nodes.end());
        if (model != parentedWorldSnap.nodes.end()) {
            const auto local = applyPoint(snappedWorld, parentInverse);
            model->localTransform.translation = {local.x, local.y, local.z};
            std::vector<scene::ResolvedTransform> afterWorldSnap;
            CHECK(scene::ResolveWorldTransforms(parentedWorldSnap, afterWorldSnap).ok());
            const auto modelWorldAfterSnap = std::find_if(afterWorldSnap.begin(), afterWorldSnap.end(),
                [](const auto& item) { return item.nodeId == "ship-01"; });
            CHECK(modelWorldAfterSnap != afterWorldSnap.end());
            if (modelWorldAfterSnap != afterWorldSnap.end()) {
                CHECK(std::fabs(modelWorldAfterSnap->world.m[3][0] - snappedWorld.x) < 1e-4f);
                CHECK(std::fabs(modelWorldAfterSnap->world.m[3][1] - snappedWorld.y) < 1e-4f);
                CHECK(std::fabs(modelWorldAfterSnap->world.m[3][2] - snappedWorld.z) < 1e-4f);
            }
        }
    }

    auto parentedRotationSnap = SceneFixture();
    float snappedRotationDelta = 0;
    constexpr float snapPi = 3.14159265358979323846f;
    CHECK(render::SnapRotationDelta(8.0f * snapPi / 180.0f, 15.0f, snappedRotationDelta));
    scene::Quaternion parentedSnappedRotation;
    CHECK(scene::RotateLocalAxis(parentedRotationSnap.nodes[2].localTransform.rotation, 2,
                                 snappedRotationDelta, parentedSnappedRotation).ok());
    parentedRotationSnap.nodes[2].localTransform.rotation = parentedSnappedRotation;
    std::vector<scene::ResolvedTransform> parentedRotationWorld;
    CHECK(scene::ResolveWorldTransforms(parentedRotationSnap, parentedRotationWorld).ok());

    auto parentedScaleSnap = SceneFixture();
    float parentedSnappedScale = 0;
    CHECK(render::AxisScaleFromParameter(parentedScaleSnap.nodes[2].localTransform.scale.x,
                                         0, 0.26f, 1.0f, parentedSnappedScale, 0.1f));
    CHECK(scene::SetLocalScaleAxis(parentedScaleSnap.nodes[2].localTransform, 0,
                                   parentedSnappedScale).ok());
    CHECK(std::fabs(parentedScaleSnap.nodes[2].localTransform.scale.x - 1.55f) < 1e-5f);
    std::vector<scene::ResolvedTransform> parentedScaleWorld;
    CHECK(scene::ResolveWorldTransforms(parentedScaleSnap, parentedScaleWorld).ok());

    auto snappedGestureBase = SceneFixture();
    auto snappedGestureCurrent = snappedGestureBase;
    scene::DocumentHistory snappedGestureHistory;
    CHECK(snappedGestureHistory.RecordBefore(snappedGestureBase).ok());
    for (const float pointerDelta : {0.06f, 0.26f, 0.44f}) {
        float snappedScaleDelta = 0;
        CHECK(render::SnapScaleDelta(pointerDelta, 0.1f, snappedScaleDelta));
        auto model = std::find_if(snappedGestureCurrent.nodes.begin(), snappedGestureCurrent.nodes.end(),
                                  [](const auto& node) { return node.id == "ship-01"; });
        CHECK(model != snappedGestureCurrent.nodes.end());
        if (model != snappedGestureCurrent.nodes.end())
            CHECK(scene::SetLocalScaleAxis(model->localTransform, 0,
                snappedGestureBase.nodes[2].localTransform.scale.x + snappedScaleDelta).ok());
        CHECK(snappedGestureHistory.retainedEntries() == 1);
    }
    CHECK(std::fabs(snappedGestureCurrent.nodes[2].localTransform.scale.x - 1.65f) < 1e-5f);
    scene::Document snappedGestureUndo;
    CHECK(snappedGestureHistory.Undo(snappedGestureCurrent, snappedGestureUndo).ok());
    CHECK(snappedGestureUndo == snappedGestureBase);
    scene::Document snappedGestureRedo;
    CHECK(snappedGestureHistory.Redo(snappedGestureUndo, snappedGestureRedo).ok());
    CHECK(snappedGestureRedo == snappedGestureCurrent);
    scene::DocumentHistory snappedGestureCancelHistory;
    CHECK(snappedGestureCancelHistory.RecordBefore(snappedGestureBase).ok());
    scene::Document snappedGestureCanceled;
    CHECK(snappedGestureCurrent != snappedGestureBase);
    CHECK(snappedGestureCancelHistory.CancelLatestChange(snappedGestureBase, snappedGestureCanceled).ok());
    CHECK(snappedGestureCanceled == snappedGestureBase);

    history.Clear();
    const auto sentinelDocument = historyRestored;
    CHECK(!history.Undo(historyRestored, historyRestored).ok());
    CHECK(historyRestored == sentinelDocument);
    auto boundedHistoryDocument = historyBase;
    for (size_t i = 0; i < scene::DocumentHistory::kMaxEntries + 8; ++i) {
        CHECK(history.RecordBefore(boundedHistoryDocument).ok());
        boundedHistoryDocument.nodes[2].localTransform.translation.x += 0.001f;
    }
    CHECK(history.retainedEntries() <= scene::DocumentHistory::kMaxEntries);
    CHECK(history.retainedBytes() <= scene::DocumentHistory::kMaxBytes);

    scene::Document unknownFields;
    CHECK(!scene::Deserialize(R"({"format":"scuba.scene","format":"scuba.scene","schema_version":1,"scene_id":"x","name":"x","nodes":[]})", unknownFields).ok());
    CHECK(!scene::Deserialize(R"({"format":"scuba.scene","schema_version":1,"scene_id":"x","name":"x","nodes":[],"extra":1})", unknownFields).ok());
    CHECK(!scene::Deserialize(R"({"format":"scuba.scene","schema_version":2,"scene_id":"x","name":"x","nodes":[]})", unknownFields).ok());

    // ProjectStore requires an existing directory and refuses a symlinked scene file when supported.
    CHECK(!scene::ProjectStore::Open(projectRoot / "missing", readOnlyRoots).has_value());
    const fs::path outside = root / "outside.scene.json";
    WriteFile(outside, encoded);
    std::error_code symlinkError;
    fs::create_symlink(outside, projectRoot / "linked.scene.json", symlinkError);
    if (!symlinkError) {
        CHECK(!project->Load("linked", reopened).ok());
        CHECK(project->ListScenes(projectScenes).ok());
        CHECK(std::find(projectScenes.begin(), projectScenes.end(), "linked") == projectScenes.end());
    }
}

}  // namespace

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    const fs::path root = TempRoot();
    TestPaths();
    TestStack();
    TestPagedListing();
    TestDirectory(root);
    TestPack(root);
    TestZip(root);
    TestOverlay();
    TestJobs();
    TestFbx();
    TestLoader();
    TestPicking();
    TestTransformGizmoMath();
    TestSceneDocument(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    return CheckExit("test_engine_core");
}
