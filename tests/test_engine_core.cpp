// SPDX-License-Identifier: GPL-3.0-only
// Native engine core: VFS backends and mount stack, sco::vfs overlay, job system, model decode and
// the async loader, picking. No window and no GPU.
#include "check.h"

#include "scuba/asset/model_loader.h"
#include "scuba/core/job_system.h"
#include "scuba/render/picking.h"
#include "scuba/vfs/pack.h"
#include "scuba/vfs/virtual_file_system.h"

#include "assets/formats.h"
#include "miniz.h"
#include "sco/vfs.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
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

    // Equal priority: the later mount wins.
    const auto later = v.Mount(vfs::MemoryFileSystem({{"ui/a.txt", B("later a")}}), {"", 10, "later"});
    CHECK(Read(v, "ui/a.txt") == "later a");

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

void TestLoader() {
    vfs::VirtualFileSystem v;
    const auto glb = assets::MakeGlb({Triangle()});
    v.Mount(vfs::MemoryFileSystem({{"models/tri.glb", glb},
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
        CHECK(loader.Info(broken).state == asset::LoadState::Failed);
        CHECK(loader.Info(fbx).state == asset::LoadState::Failed && loader.Info(fbx).error.find("FBX") != std::string::npos);
        CHECK(loader.Info(missing).state == asset::LoadState::Failed);
        CHECK(loader.Info(bad).state == asset::LoadState::Failed);
        CHECK(loader.TakeCompleted().size() == 6);
        CHECK(loader.TakeCompleted().empty());
        CHECK(loader.Request("models/broken.glb") != broken);   // failures are retried
        jobs.WaitIdle();
    }
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

    render::PickTarget a{1, &model, {}}, b{2, &model, {}};
    a.transform.position = {0, 0, 0};
    b.transform.position = {10, 0, 0};
    b.transform.scale = 2;
    const render::PickTarget targets[] = {a, b};
    auto hit = render::Pick({{0, 0, 10}, {0, 0, -1}}, targets);
    CHECK(hit && hit->id == 1 && std::fabs(hit->distance - 9.0f) < 1e-4f);
    hit = render::Pick({{10, 0, 10}, {0, 0, -1}}, targets);
    CHECK(hit && hit->id == 2 && std::fabs(hit->distance - 8.0f) < 1e-4f);   // scaled cube top at z = 2
    CHECK(!render::Pick({{5, 0, 10}, {0, 0, -1}}, targets));
    auto ground = render::RayPlaneZ({{0, 0, 5}, {0, 0.6f, -0.8f}}, 0);
    CHECK(ground && std::fabs(ground->y - 3.75f) < 1e-4f);

    render::OrbitCamera cam;
    cam.target = {0, 0, 0};
    const render::Rect vp{0, 0, 800, 600};
    const auto center = cam.ScreenRay(400, 300, vp);
    const auto eye = cam.Eye();
    const float len = std::sqrt(eye.x * eye.x + eye.y * eye.y + eye.z * eye.z);
    CHECK(std::fabs(center.dir.x + eye.x / len) < 1e-4f && std::fabs(center.dir.z + eye.z / len) < 1e-4f);
}

}  // namespace

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    const fs::path root = TempRoot();
    TestPaths();
    TestStack();
    TestDirectory(root);
    TestPack(root);
    TestZip(root);
    TestOverlay();
    TestJobs();
    TestLoader();
    TestPicking();
    std::error_code ec;
    fs::remove_all(root, ec);
    return CheckExit("test_engine_core");
}
