// SPDX-License-Identifier: GPL-3.0-only
// Native engine core: VFS backends and mount stack, sco::vfs overlay, job system, model decode and
// the async loader, picking. No window and no GPU.
#include "check.h"

#include "scuba/asset/model_loader.h"
#include "scuba/core/job_system.h"
#include "scuba/render/picking.h"
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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
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
    std::vector<scene::ResolvedTransform> resolved;
    CHECK(scene::ResolveWorldTransforms(document, resolved).ok());
    CHECK(resolved.size() == document.nodes.size());
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
    auto project = scene::ProjectStore::Open(projectRoot, readOnlyRoots, &openStatus);
    CHECK(project && openStatus.ok());
    if (!project) return;
    CHECK(!scene::ProjectStore::Open(assetRoot, readOnlyRoots).has_value());
    CHECK(!scene::ProjectStore::Open(assetRoot / "nested", readOnlyRoots).has_value());

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
    TestDirectory(root);
    TestPack(root);
    TestZip(root);
    TestOverlay();
    TestJobs();
    TestFbx();
    TestLoader();
    TestPicking();
    TestSceneDocument(root);
    std::error_code ec;
    fs::remove_all(root, ec);
    return CheckExit("test_engine_core");
}
