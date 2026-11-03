// SPDX-License-Identifier: GPL-3.0-only
// scuba-engine: the native editor. One main thread (window, sco-core game thread, UI, scene), a
// render thread inside the renderer, and a job pool for asset intake.
//
//   scuba-engine [--content DIR] [--mount PATH[=MOUNTPOINT][@PRIORITY]]... [--plugins DIR | --no-plugins]
//                [--data DIR] [--product DIR] [--project DIR [--scene ID]] [--open VFS_PATH]...
//                [--warp] [--debug-layer] [--no-vsync]
//                [--hidden] [--frames N] [--scenario FILE] [--diegetic [PANEL_ID]] [--readback FILE]
//                [--export DIR | --export-pack FILE]
//
// --product DIR mounts a product distribution root: <DIR>/data/plugins is the roster, <DIR>/data is
// the content mount and sco.storage root. It is fail-closed: a refused plugin, a missing
// data/plugins or an unusable root stops the editor with exit code 1 and prints the roster.
//
// --frames N renders N frames and exits: 0 when every --open model became resident and nothing
// was logged at error level, 1 otherwise (the smoke test uses it with --warp --hidden). With
// --hidden the frames go to an off-screen target and nothing is presented, so the smoke path runs
// where there is no desktop at all.
//
// --scenario FILE runs a bounded scenario (the same steps scuba-hostd accepts) against the mock
// game before the first frame, prints each step, and exits 1 if any step did not match. The
// Scenario dock runs one on demand from the UI.
//
// --diegetic [PANEL_ID] draws a plugin panel in its own UI context on a world-space quad.
// Without an ID it selects the first published panel. --readback FILE writes a binary PPM;
// pixel statistics are descriptive, not proof that a plugin or GPU path worked.
//
// --export DIR / --export-pack FILE write a product distribution from the mounted --product root
// and exit without creating a window: <DIR>/data (the roster included) or one .scpak of the same
// tree. Both refuse an occupied destination and a roster folder without a plugin.ini.
#include "scuba/asset/model_loader.h"
#include "scuba/asset/browser.h"
#include "scuba/core/job_system.h"
#include "scuba/core/log.h"
#include "scuba/platform/window.h"
#include "scuba/render/camera.h"
#include "scuba/product/export.h"
#include "scuba/render/picking.h"
#include "scuba/render/renderer.h"
#include "scuba/render/transform_gizmo.h"
#include "scuba/scene/scene_document.h"
#include "scuba/sco/sco_host.h"
#include "scuba/ui/input_router.h"
#include "scuba/ui/plugin_surface.h"
#include "scuba/ui/designer.h"
#include "scuba/ui/net_sandbox.h"
#include "scuba/ui/ui_layer.h"
#include "scuba/vfs/pack.h"
#include "scuba/vfs/virtual_file_system.h"

#include "hostd/scenario_runner.h"

#include "imgui.h"
#include "sco/widgets_imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace scuba;

namespace {

std::atomic<int> gErrors{0};

void LogSink(const log::Line& line) {
    if (line.level == log::Level::Error) ++gErrors;
    const std::string text = std::string("[") + log::LevelName(line.level) + "] " + line.channel + ": " + line.text + "\n";
    OutputDebugStringA(text.c_str());
    std::fputs(text.c_str(), stderr);
}

struct Args {
    fs::path content = SCUBA_DEFAULT_CONTENT_DIR;
    fs::path plugins = SCUBA_DEFAULT_PLUGIN_ROOT;
    fs::path data;
    fs::path product;
    fs::path project;
    std::string sceneId = "main";
    bool sceneSet = false;
    bool contentSet = false, pluginsSet = false, dataSet = false;
    struct MountArg { std::string path, mountPoint; int priority = 10; };
    std::vector<MountArg> mounts;
    std::vector<std::string> open;
    fs::path scenario;
    fs::path exportDir, exportPack;
    fs::path readback;
    bool diegetic = false;
    std::string panelId;
    bool warp = false, debugLayer = false, vsync = true, hidden = false, plugins_on = true;
    int frames = 0;
};

std::string Utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0 && WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr) == 0)
        return {};
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

fs::path PathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::optional<Args> ParseArgs() {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) a.push_back(Utf8(wargv[i]));
    LocalFree(wargv);
    Args args;
    for (size_t i = 0; i < a.size(); ++i) {
        auto next = [&]() -> std::optional<std::string> { return i + 1 < a.size() ? std::optional<std::string>(a[++i]) : std::nullopt; };
        const std::string& k = a[i];
        if (k == "--content") { if (auto v = next()) { args.content = PathFromUtf8(*v); args.contentSet = true; } else return std::nullopt; }
        else if (k == "--plugins") { if (auto v = next()) { args.plugins = PathFromUtf8(*v); args.pluginsSet = true; } else return std::nullopt; }
        else if (k == "--no-plugins") { args.plugins_on = false; args.pluginsSet = true; }
        else if (k == "--data") { if (auto v = next()) { args.data = PathFromUtf8(*v); args.dataSet = true; } else return std::nullopt; }
        else if (k == "--product") { if (auto v = next()) args.product = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--project") { if (auto v = next()) args.project = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--scene") { if (auto v = next()) { args.sceneId = *v; args.sceneSet = true; } else return std::nullopt; }
        else if (k == "--open") { if (auto v = next()) args.open.push_back(*v); else return std::nullopt; }
        else if (k == "--scenario") { if (auto v = next()) args.scenario = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--export") { if (auto v = next()) args.exportDir = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--export-pack") { if (auto v = next()) args.exportPack = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--readback") { if (auto v = next()) args.readback = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--diegetic") {
            args.diegetic = true;
            if (i + 1 < a.size() && !a[i + 1].starts_with("--")) args.panelId = a[++i];
        }
        else if (k == "--frames") { if (auto v = next()) args.frames = std::max(1, std::atoi(v->c_str())); else return std::nullopt; }
        else if (k == "--mount") {
            auto v = next();
            if (!v) return std::nullopt;
            Args::MountArg m;
            std::string s = *v;
            if (auto at = s.rfind('@'); at != std::string::npos && at > 2) { m.priority = std::atoi(s.c_str() + at + 1); s.resize(at); }
            if (auto eq = s.find('='); eq != std::string::npos) { m.mountPoint = s.substr(eq + 1); s.resize(eq); }
            m.path = s;
            args.mounts.push_back(m);
        }
        else if (k == "--warp") args.warp = true;
        else if (k == "--debug-layer") args.debugLayer = true;
        else if (k == "--no-vsync") args.vsync = false;
        else if (k == "--hidden") args.hidden = true;
        else return std::nullopt;
    }
    // A product profile derives all three of its roots; mixing them would be ambiguous.
    if (!args.product.empty() && (args.pluginsSet || args.dataSet || args.contentSet)) return std::nullopt;
    if (args.sceneSet && args.project.empty()) return std::nullopt;
    // An export is a product distribution or nothing: it reads the mounted root, so it needs one,
    // and the two destinations are different artifacts.
    if (!args.exportDir.empty() && !args.exportPack.empty()) return std::nullopt;
    if ((!args.exportDir.empty() || !args.exportPack.empty()) && args.product.empty()) return std::nullopt;
    if ((!args.exportDir.empty() || !args.exportPack.empty()) && !args.project.empty()) return std::nullopt;
    return args;
}

// Mounts a directory, .zip or .scpak by what the path is.
Status MountPath(vfs::VirtualFileSystem& vfs, const fs::path& path, const std::string& mountPoint, int priority,
                 vfs::MountId* mountedId = nullptr) {
    Status st;
    std::unique_ptr<vfs::FileSystem> backend;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (fs::is_directory(path)) backend = vfs::OpenDirectory(path, st);
    else if (ext == ".zip") backend = vfs::OpenZip(path, st);
    else if (ext == ".scpak") backend = vfs::OpenPack(path, st);
    else return Status::Fail("not a directory, .zip or .scpak: " + path.generic_string());
    if (!backend) return st;
    const vfs::MountId id = vfs.Mount(std::move(backend), {mountPoint, priority, path.filename().generic_string()}, &st);
    if (!st) return st;
    if (id == 0) return Status::Fail("VFS refused the asset mount");
    if (mountedId) *mountedId = id;
    if (st) log::Info("vfs", "mounted " + path.generic_string() + (mountPoint.empty() ? "" : " at " + mountPoint) + " (priority " + std::to_string(priority) + ")");
    return st;
}

// ---- scene -------------------------------------------------------------------------------------

struct Instance {
    uint32_t id = 0;
    std::string nodeId;
    std::string name;
    std::string assetPath;
    asset::ModelHandle handle = 0;
    render::GpuModel gpu = 0;
    std::shared_ptr<const asset::ModelData> model;
    render::Float4x4 world;
    render::Material material;
    bool autoPlaceOnLoad = false;
    uint64_t autoPlaceHistoryRevision = 0;
};

enum class TransformGizmoMode : uint8_t { Move, Scale, Rotate };

struct TransformGizmoGeometry {
    render::ScreenPoint center;
    render::Float3 worldCenter;
    std::array<render::Float3, 3> directions;
    std::array<render::ScreenSegment, 3> axes;
    std::array<render::ScreenSegment, 3> hitHandles;
    std::array<render::Float3, 3> rotationBasisU;
    std::array<render::Float3, 3> rotationBasisV;
    std::array<render::ScreenSegment, render::kRotationRingSegments * 3> rotationHandles;
    float worldLength = 0;
};

struct Placement {
    bool active = false;
    bool hasPreview = false;
    std::string assetPath;
    std::optional<std::string> parentId;
    asset::ModelHandle handle = 0;
    render::GpuModel gpu = 0;
    std::shared_ptr<const asset::ModelData> model;
    render::Float4x4 world;
    scene::LocalTransform localTransform;
    std::string status;
};

render::Float3 TransformPoint(render::Float3 point, const render::Float4x4& matrix) {
    return {point.x * matrix.m[0][0] + point.y * matrix.m[1][0] + point.z * matrix.m[2][0] + matrix.m[3][0],
            point.x * matrix.m[0][1] + point.y * matrix.m[1][1] + point.z * matrix.m[2][1] + matrix.m[3][1],
            point.x * matrix.m[0][2] + point.y * matrix.m[1][2] + point.z * matrix.m[2][2] + matrix.m[3][2]};
}

struct Editor {
    struct MountedSource {
        fs::path path;
        bool directory = false;
    };

    struct PreparedProject {
        std::optional<scene::ProjectStore> store;
        fs::path root;
        scene::Document document;
        scene::DocumentDirtyState dirty;
        bool saved = false;
    };

    vfs::VirtualFileSystem vfs;
    JobSystem jobs;
    std::unique_ptr<asset::ModelLoader> loader;
    std::unique_ptr<render::Renderer> renderer;
    ScoHost sco;
    ui::UiLayer ui;
    // Destroy before ui: the surface borrows the main context's font atlas.
    std::unique_ptr<ui::PluginSurface> surface;
    ui::DesignerDock designer;
    std::unique_ptr<ui::NetSandbox> netSandbox;
    bool showSurface = true, showNetSandbox = false, surfaceEnabled = false;
    bool surfaceFocused = false, surfaceDragging = false;
    std::string surfacePanel, surfaceStatus;
    render::OrbitCamera camera;

    render::Float4x4 SurfaceWorld() const {
        const auto view = camera.View();
        const auto eye = camera.Eye();
        render::Float4x4 world{};
        // View COLUMNS are world-space camera axes; quad world ROWS are its local axes.
        for (int c = 0; c < 3; ++c) {
            world.m[0][c] = view.m[c][0] * 2.0f;
            world.m[1][c] = view.m[c][1] * 1.5f;
            world.m[2][c] = view.m[c][2];
        }
        world.m[0][3] = world.m[1][3] = world.m[2][3] = 0;
        world.m[3][0] = eye.x - view.m[0][2] * 3.0f;
        world.m[3][1] = eye.y - view.m[1][2] * 3.0f;
        world.m[3][2] = eye.z - view.m[2][2] * 3.0f;
        world.m[3][3] = 1;
        return world;
    }

    scene::Document document;
    scene::DocumentHistory sceneHistory;
    scene::DocumentDirtyState documentDirty;
    scene::SceneActionGate sceneActionGate;
    std::optional<scene::ProjectStore> projectStore;
    fs::path projectRoot;
    std::map<vfs::MountId, MountedSource> mountedSources;
    std::vector<Instance> instances;
    std::unordered_map<std::string, render::Float4x4> worldByNode;
    uint32_t nextInstance = 1;
    uint64_t historyRevision = 0;
    std::string selectedNode;
    std::map<asset::ModelHandle, render::GpuModel> uploaded;   // one GPU copy per loaded model
    std::vector<asset::ModelHandle> requested;
    Placement placement;
    std::string selectedAssetPath;
    std::string sceneStatus;
    bool inspectorUndoCaptured = false;
    std::string inspectorUndoNode;
    scene::Vec3 inspectorRotationDegrees;
    std::string inspectorRotationNode;
    bool inspectorRotationInitialized = false;
    bool inspectorRotationActive = false;
    bool transformGizmoDragging = false;
    bool transformGizmoGestureActive = false;
    TransformGizmoMode gizmoMode = TransformGizmoMode::Move;
    render::GizmoSnapSettings gizmoSnapSettings;
    int transformGizmoAxis = -1;
    char renameBuffer[129] = "";  // scene node names are capped at 128 UTF-8 bytes
    std::string renameTarget;
    std::string renameSourceName;
    std::string reparentTarget;
    std::string reparentTargetNode;
    bool renameBufferDirty = false;
    bool showSceneBrowser = false;
    char newSceneId[65] = "";
    std::vector<std::string> projectScenes;
    std::string selectedProjectScene;
    std::string sceneBrowserStatus;
    bool pendingSceneConfirmation = false;
    bool showProjectOpenDialog = false;
    bool createProjectDialog = false;
    char projectPathBuffer[1024] = "";
    std::string projectOpenStatus;
    bool frameOnLoad = true;

    ~Editor() {
        if (netSandbox && !netSandbox->Shutdown())
        std::fprintf(stderr, "scuba-engine: network sandbox peer exited uncleanly; child termination was confirmed\n");
#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
        if (!sco.StopChecked()) std::_Exit(EXIT_FAILURE);
#else
        sco.Stop();
#endif
    }

    std::vector<fs::path> MountedSourcePaths() const {
        std::vector<fs::path> paths;
        paths.reserve(mountedSources.size());
        for (const auto& [id, source] : mountedSources) {
            (void)id;
            paths.push_back(source.path);
        }
        return paths;
    }

    std::vector<fs::path> MountedDirectoryPaths() const {
        std::vector<fs::path> paths;
        paths.reserve(mountedSources.size());
        for (const auto& [id, source] : mountedSources) {
            (void)id;
            if (source.directory) paths.push_back(source.path);
        }
        return paths;
    }

    Status MountAssetSource(const fs::path& path, const std::string& mountPoint, int priority) {
        std::error_code ec;
        const fs::path canonical = fs::canonical(path, ec);
        if (ec) return Status::Fail("asset source cannot be resolved: " + ec.message());
        const bool directory = fs::is_directory(canonical, ec);
        if (ec) return Status::Fail("asset source cannot be inspected: " + ec.message());
        if (projectStore) {
            auto sources = MountedSourcePaths();
            sources.push_back(canonical);
            if (auto status = scene::ProjectStore::ValidateSourceSeparation(projectRoot, sources); !status)
                return status;
        }

        vfs::MountId id = 0;
        if (auto status = MountPath(vfs, canonical, mountPoint, priority, &id); !status) return status;
        mountedSources.emplace(id, MountedSource{canonical, directory});
        return Status::Ok();
    }

    // panels
    bool showContent = true, showMounts = true, showScene = true, showInspector = true, showPlugins = true,
         showLoader = true, showLog = true, showStats = true, showScenario = false, showProduct = false,
         showDemo = false;
    char scenarioPath[512] = "";
    fs::path productRoot;
    char exportPath[512] = "";
    std::string exportStatus;
    bool exportPack = false;
    std::vector<hostd::ScenarioStepResult> scenarioResults;
    std::string scenarioStatus;
    bool scenarioPassed = false;
    char mountPath[512] = "";
    char mountPoint[128] = "";
    int mountPriority = 10;
    char contentFilter[128] = "";
    std::vector<vfs::Entry> contentCatalog;
    asset::BrowserContents contentBrowserView;
    std::string contentDirectory;
    std::string contentBrowserViewDirectory;
    std::string contentBrowserViewSearch;
    std::string contentBrowserStatus;
    bool contentCatalogLoaded = false;
    bool contentCatalogRejected = false;
    bool contentBrowserViewValid = false;
    bool contentTileView = true;
    uint64_t logSeen = 0;
    std::vector<log::Line> logLines;

    Instance* Find(uint32_t id) {
        for (auto& i : instances) if (i.id == id) return &i;
        return nullptr;
    }

    scene::Node* FindNode(std::string_view id) {
        for (auto& node : document.nodes) if (node.id == id) return &node;
        return nullptr;
    }

    const scene::Node* FindNode(std::string_view id) const {
        for (const auto& node : document.nodes) if (node.id == id) return &node;
        return nullptr;
    }

    bool IsDescendantOrSelf(std::string_view nodeId, std::string_view ancestorId) const {
        const scene::Node* cursor = FindNode(nodeId);
        while (cursor) {
            if (cursor->id == ancestorId) return true;
            cursor = cursor->parentId ? FindNode(*cursor->parentId) : nullptr;
        }
        return false;
    }

    Instance* FindNodeInstance(std::string_view id) {
        for (auto& inst : instances) if (inst.nodeId == id) return &inst;
        return nullptr;
    }

    const render::Float4x4* World(std::string_view id) const {
        const auto it = worldByNode.find(std::string(id));
        return it == worldByNode.end() ? nullptr : &it->second;
    }

    void TrackRequestedModel(asset::ModelHandle handle) {
        if (handle != 0 && std::find(requested.begin(), requested.end(), handle) == requested.end())
            requested.push_back(handle);
    }

    asset::ModelHandle RequestModel(const std::string& path) {
        const asset::ModelHandle h = loader->Request(path, JobPriority::High);
        TrackRequestedModel(h);
        return h;
    }

    std::vector<asset::ModelHandle> FailedSceneAssetHandles() const {
        std::vector<asset::ModelHandle> failed;
        if (!loader) return failed;
        std::unordered_set<asset::ModelHandle> seen;
        for (const auto& inst : instances) {
            if (inst.model || inst.handle == 0 || !seen.insert(inst.handle).second) continue;
            if (loader->Info(inst.handle).state == asset::LoadState::Failed)
                failed.push_back(inst.handle);
        }
        return failed;
    }

    void RetryFailedSceneAssets() {
        const auto failedHandles = FailedSceneAssetHandles();
        if (failedHandles.empty()) {
            sceneStatus = "No failed scene assets to retry";
            return;
        }

        size_t retried = 0;
        for (const asset::ModelHandle failedHandle : failedHandles) {
            const asset::ModelHandle retryHandle = loader->Retry(failedHandle, JobPriority::High);
            if (retryHandle == 0) continue;
            TrackRequestedModel(retryHandle);
            for (auto& inst : instances) {
                if (inst.handle != failedHandle || inst.model) continue;
                inst.handle = retryHandle;
                inst.gpu = 0;
                ++retried;
            }
            if (placement.active && placement.handle == failedHandle && !placement.model) {
                placement.handle = retryHandle;
                placement.gpu = 0;
                placement.status = "Retrying preview load · Esc cancels";
            }
        }
        sceneStatus = retried == 0
            ? "Could not queue a retry; failed scene objects remain in place"
            : "Retrying failed asset loads for " + std::to_string(retried) + " scene object(s)";
    }

    void AttachCachedModel(Instance& inst) {
        if (const auto it = uploaded.find(inst.handle); it != uploaded.end()) {
            inst.gpu = it->second;
            inst.model = loader->Get(inst.handle);
            return;
        }
        if (auto model = loader->Get(inst.handle)) {
            inst.model = std::move(model);
            inst.gpu = renderer->Upload(inst.model);
            if (inst.gpu) uploaded[inst.handle] = inst.gpu;
        }
    }

    std::string NewNodeId() {
        static std::random_device random;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const uint64_t value = (static_cast<uint64_t>(random()) << 32u) | random();
            char suffix[17]{};
            std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(value));
            const std::string id = "node-" + std::string(suffix);
            if (!FindNode(id)) return id;
        }
        return {};
    }

    Status ResolveDocument(const scene::Document& candidate,
                          std::unordered_map<std::string, render::Float4x4>& worlds) const {
        std::vector<scene::ResolvedTransform> resolved;
        if (auto status = scene::ResolveWorldTransforms(candidate, resolved); !status) return status;
        std::unordered_map<std::string, render::Float4x4> result;
        result.reserve(resolved.size());
        for (const auto& item : resolved) result.emplace(item.nodeId, item.world);
        worlds = std::move(result);
        return Status::Ok();
    }

    bool BuildGizmoGeometry(TransformGizmoGeometry& geometry) const {
        if (selectedNode.empty()) return false;
        const auto* node = FindNode(selectedNode);
        const auto* world = World(selectedNode);
        if (!node || !world) return false;
        const auto viewport = ui.viewport();
        if (viewport.w <= 1 || viewport.h <= 1) return false;
        const auto origin = TransformPoint({0, 0, 0}, *world);
        geometry.worldCenter = origin;
        const auto view = camera.View();
        const auto projection = camera.Projection(viewport.w / viewport.h);
        if (!render::ProjectPoint(origin, view, projection, viewport, geometry.center)) return false;

        constexpr render::Float3 worldAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (gizmoMode != TransformGizmoMode::Move) {
            render::Float4x4 localRotation;
            const auto& q = node->localTransform.rotation;
            if (!render::AffineFromTrs({}, {q.x, q.y, q.z, q.w}, {1, 1, 1}, localRotation)) return false;
            render::Float4x4 localAxesToWorld = localRotation;
            if (node->parentId) {
                const auto* parentWorld = World(*node->parentId);
                if (!parentWorld) return false;
                localAxesToWorld = render::Multiply(localRotation, *parentWorld);
            }
            for (size_t axis = 0; axis < 3; ++axis) {
                if (gizmoMode == TransformGizmoMode::Scale) {
                    if (!render::NormalizeTransformedDirection(worldAxes[axis], localAxesToWorld,
                                                              geometry.directions[axis]))
                        return false;
                } else {
                    const size_t uAxis = (axis + 1) % 3;
                    const size_t vAxis = (axis + 2) % 3;
                    if (!render::TransformDirection(worldAxes[uAxis], localAxesToWorld,
                                                    geometry.rotationBasisU[axis]) ||
                        !render::TransformDirection(worldAxes[vAxis], localAxesToWorld,
                                                    geometry.rotationBasisV[axis]))
                        return false;
                }
            }
        } else {
            for (size_t axis = 0; axis < 3; ++axis) geometry.directions[axis] = worldAxes[axis];
        }

        const auto eye = camera.Eye();
        const float dx = origin.x - eye.x, dy = origin.y - eye.y, dz = origin.z - eye.z;
        const float cameraDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(cameraDistance) || cameraDistance <= 0) return false;
        const float worldLength = std::clamp(
            2.0f * std::tan(camera.fovY * 0.5f) * cameraDistance * 90.0f / viewport.h,
            0.01f, 100000.0f);
        if (!std::isfinite(worldLength) || worldLength <= 0) return false;
        geometry.worldLength = worldLength;
        if (gizmoMode == TransformGizmoMode::Rotate) {
            for (size_t axis = 0; axis < 3; ++axis) {
                auto ring = std::span<render::ScreenSegment>(geometry.rotationHandles).subspan(
                    axis * render::kRotationRingSegments, render::kRotationRingSegments);
                if (!render::ProjectRotationRing(origin, geometry.rotationBasisU[axis],
                                                 geometry.rotationBasisV[axis], worldLength,
                                                 view, projection, viewport, ring))
                    for (auto& segment : ring) segment = {geometry.center, geometry.center};
            }
            return true;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto direction = geometry.directions[axis];
            const render::Float3 endpoint{origin.x + direction.x * worldLength,
                                          origin.y + direction.y * worldLength,
                                          origin.z + direction.z * worldLength};
            render::ScreenPoint end;
            if (!render::ProjectPoint(endpoint, view, projection, viewport, end)) {
                geometry.axes[axis] = {geometry.center, geometry.center};
                geometry.hitHandles[axis] = {geometry.center, geometry.center};
                continue;
            }
            geometry.axes[axis] = {geometry.center, end};
            constexpr float kHandleStartFraction = 0.18f;
            const render::ScreenPoint handleStart{
                geometry.center.x + (end.x - geometry.center.x) * kHandleStartFraction,
                geometry.center.y + (end.y - geometry.center.y) * kHandleStartFraction,
                geometry.center.depth + (end.depth - geometry.center.depth) * kHandleStartFraction};
            geometry.hitHandles[axis] = {handleStart, end};
        }
        return true;
    }

    void RefreshInstances() {
        std::unordered_set<std::string> keep;
        keep.reserve(document.nodes.size());
        for (const auto& node : document.nodes) {
            if (node.kind != scene::NodeKind::ModelInstance) continue;
            keep.insert(node.id);
            Instance* inst = FindNodeInstance(node.id);
            if (!inst) {
                Instance added;
                added.id = nextInstance++;
                if (added.id == 0) added.id = nextInstance++;
                added.nodeId = node.id;
                added.name = node.name;
                added.assetPath = node.assetPath;
                added.handle = RequestModel(node.assetPath);
                AttachCachedModel(added);
                if (const auto it = worldByNode.find(node.id); it != worldByNode.end()) added.world = it->second;
                instances.push_back(std::move(added));
                continue;
            }
            inst->name = node.name;
            if (inst->assetPath != node.assetPath) {
                inst->assetPath = node.assetPath;
                inst->handle = RequestModel(node.assetPath);
                inst->gpu = 0;
                inst->model.reset();
                AttachCachedModel(*inst);
            }
            if (const auto it = worldByNode.find(node.id); it != worldByNode.end()) inst->world = it->second;
        }
        instances.erase(std::remove_if(instances.begin(), instances.end(), [&](const Instance& inst) {
            return !keep.contains(inst.nodeId);
        }), instances.end());
        if (!selectedNode.empty() && !FindNode(selectedNode)) selectedNode.clear();
    }

    Status UpdateDirtyState() {
        return documentDirty.Update(document);
    }

    Status SetSavedBaseline(bool exists) {
        return documentDirty.SetBaseline(document, exists);
    }

    void SetHistoryStatus(std::string_view action) {
        if (auto status = UpdateDirtyState(); !status) {
            sceneStatus = std::string(action) + " failed: " + status.error;
            return;
        }
        sceneStatus = std::string(action) + (documentDirty.dirty() ? " · unsaved changes" : " · matches saved scene");
    }

    Status InstallDocument(scene::Document candidate, bool recordUndo = false) {
        std::unordered_map<std::string, render::Float4x4> worlds;
        if (auto status = ResolveDocument(candidate, worlds); !status) return status;
        if (recordUndo)
            if (auto status = sceneHistory.RecordBefore(document); !status) return status;
        document = std::move(candidate);
        worldByNode = std::move(worlds);
        RefreshInstances();
        sceneStatus = "Scene loaded";
        if (placement.active && placement.parentId && !FindNode(*placement.parentId))
            CancelPlacement("Placement canceled because its parent node was removed");
        return Status::Ok();
    }

    Status CommitDocument(scene::Document candidate) {
        if (candidate == document) return Status::Ok();
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        if (auto status = InstallDocument(std::move(candidate), true); !status) {
            sceneStatus = "Edit refused: " + status.error;
            return status;
        }
        ++historyRevision;
        SetHistoryStatus("Edit");
        return Status::Ok();
    }

    Status CommitInteractiveDocument(scene::Document candidate, bool& undoCaptured) {
        if (candidate == document) return Status::Ok();
        const bool startsTransaction = !undoCaptured;
        if (auto status = InstallDocument(std::move(candidate), startsTransaction); !status) {
            sceneStatus = "Edit refused: " + status.error;
            return status;
        }
        undoCaptured = true;
        if (startsTransaction) ++historyRevision;
        SetHistoryStatus("Edit");
        return Status::Ok();
    }

    void UndoScene() {
        if (!sceneHistory.CanUndo()) return;
        scene::Document restored;
        if (auto status = sceneHistory.Undo(document, restored); !status) {
            sceneStatus = "Undo failed: " + status.error;
            return;
        }
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        if (auto status = InstallDocument(std::move(restored)); !status) {
            sceneStatus = "Undo could not install its saved scene: " + status.error;
            return;
        }
        ++historyRevision;
        SetHistoryStatus("Undo");
    }

    void RedoScene() {
        if (!sceneHistory.CanRedo()) return;
        scene::Document restored;
        if (auto status = sceneHistory.Redo(document, restored); !status) {
            sceneStatus = "Redo failed: " + status.error;
            return;
        }
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        if (auto status = InstallDocument(std::move(restored)); !status) {
            sceneStatus = "Redo could not install its saved scene: " + status.error;
            return;
        }
        ++historyRevision;
        SetHistoryStatus("Redo");
    }

    void AddGroup() {
        const std::string id = NewNodeId();
        if (id.empty()) { sceneStatus = "Cannot allocate a stable scene node ID"; return; }
        std::optional<std::string> parentId;
        if (const scene::Node* selected = FindNode(selectedNode); selected && selected->kind == scene::NodeKind::Group)
            parentId = selected->id;
        scene::Document candidate = document;
        if (auto status = scene::AddGroup(candidate, id, "Group", parentId); !status) {
            sceneStatus = "Group creation refused: " + status.error;
            return;
        }
        if (CommitDocument(std::move(candidate))) selectedNode = id;
    }

    void DuplicateSelected() {
        if (!FindNode(selectedNode)) return;
        scene::Document candidate = document;
        std::unordered_set<std::string> generatedIds;
        const auto generateId = [&]() {
            for (unsigned attempt = 0; attempt < 128; ++attempt) {
                std::string id = NewNodeId();
                if (!id.empty() && generatedIds.insert(id).second) return id;
            }
            return std::string{};
        };
        std::string duplicateRootId;
        if (auto status = scene::DuplicateSubtree(candidate, selectedNode, generateId, duplicateRootId); !status) {
            sceneStatus = "Duplicate refused: " + status.error;
            return;
        }
        if (CommitDocument(std::move(candidate))) {
            selectedNode = std::move(duplicateRootId);
            sceneStatus = "Subtree duplicated · unsaved";
        }
    }

    void SaveScene() {
        if (!projectStore) {
            sceneStatus = "No project session. Open a project from File or launch with --project DIR to save.";
            return;
        }
        const auto sources = MountedSourcePaths();
        if (auto status = projectStore->Save(document, sources); !status) {
            sceneStatus = "Save failed: " + status.error;
            log::Error("scene", sceneStatus);
            return;
        }
        if (auto status = SetSavedBaseline(true); !status) {
            sceneStatus = "Saved, but cannot update the editor baseline: " + status.error;
            log::Error("scene", sceneStatus);
            return;
        }
        sceneStatus = "Saved " + document.id;
        log::Info("scene", sceneStatus);
    }

    void RefreshProjectScenes() {
        if (!projectStore) {
            sceneBrowserStatus = "Open a project from File > Open project to browse scenes.";
            projectScenes.clear();
            return;
        }
        std::vector<std::string> listed;
        const auto sources = MountedSourcePaths();
        if (auto status = projectStore->ListScenes(listed, sources); !status) {
            projectScenes.clear();
            selectedProjectScene.clear();
            sceneBrowserStatus = "Cannot list project scenes: " + status.error;
            return;
        }
        projectScenes = std::move(listed);
        if (std::find(projectScenes.begin(), projectScenes.end(), selectedProjectScene) == projectScenes.end())
            selectedProjectScene.clear();
        sceneBrowserStatus.clear();
    }

    void OpenSceneBrowser() {
        if (!projectStore) {
            sceneStatus = "Open a project from File > Open project to browse scenes.";
            return;
        }
        showSceneBrowser = true;
        RefreshProjectScenes();
        selectedProjectScene = std::find(projectScenes.begin(), projectScenes.end(), document.id) != projectScenes.end()
            ? document.id : std::string{};
    }

    void OpenProjectDialog() {
        projectOpenStatus.clear();
        createProjectDialog = false;
        showProjectOpenDialog = true;
    }

    void CreateProjectDialog() {
        projectOpenStatus.clear();
        createProjectDialog = true;
        showProjectOpenDialog = true;
    }

    void RequestOpenProject() {
        const std::string pathText(projectPathBuffer);
        if (pathText.empty()) {
            projectOpenStatus = "Enter an existing project directory path.";
            return;
        }
        try {
            PreparedProject preview;
            if (auto status = PrepareProject(PathFromUtf8(pathText), "main", preview); !status) {
                projectOpenStatus = "Cannot open project: " + status.error;
                return;
            }
            projectOpenStatus.clear();
        } catch (const std::exception& error) {
            projectOpenStatus = std::string("Invalid project path: ") + error.what();
            return;
        }
        auto action = sceneActionGate.Request(
            {scene::SceneActionKind::OpenProject, pathText}, documentDirty.dirty());
        if (action) CompletePendingSceneAction(*action);
        else pendingSceneConfirmation = true;
    }

    void RequestCreateProject() {
        const std::string pathText(projectPathBuffer);
        if (pathText.empty()) {
            projectOpenStatus = "Enter a path for a new project folder.";
            return;
        }
        try {
            const auto sources = MountedSourcePaths();
            const auto readOnlyRoots = MountedDirectoryPaths();
            if (auto status = scene::ProjectStore::ValidateProjectCreateTarget(
                    PathFromUtf8(pathText), sources, readOnlyRoots); !status) {
                projectOpenStatus = "Cannot create project: " + status.error;
                return;
            }
        } catch (const std::exception& error) {
            projectOpenStatus = std::string("Invalid project path: ") + error.what();
            return;
        }
        auto action = sceneActionGate.Request(
            {scene::SceneActionKind::CreateProject, pathText}, documentDirty.dirty());
        if (action) CompletePendingSceneAction(*action);
        else pendingSceneConfirmation = true;
    }

    void RequestOpenScene(std::string sceneId) {
        if (!projectStore) return;
        if (sceneId.empty() || sceneId == document.id) return;
        auto action = sceneActionGate.Request({scene::SceneActionKind::Open, std::move(sceneId)},
                                              documentDirty.dirty());
        if (action) CompletePendingSceneAction(*action);
        else pendingSceneConfirmation = true;
    }

    void RequestQuit(bool& quit) {
        auto action = sceneActionGate.Request({scene::SceneActionKind::Quit, {}}, documentDirty.dirty());
        if (action) CompletePendingSceneAction(*action, &quit);
        else pendingSceneConfirmation = true;
    }

    void RequestCreateScene(std::string sceneId) {
        if (!projectStore) return;
        scene::Document candidate;
        candidate.id = std::move(sceneId);
        candidate.name = candidate.id;
        if (auto status = scene::Validate(candidate); !status) {
            sceneBrowserStatus = "Cannot create scene: " + status.error;
            return;
        }
        RefreshProjectScenes();
        if (!sceneBrowserStatus.empty()) return;
        if (std::find(projectScenes.begin(), projectScenes.end(), candidate.id) != projectScenes.end()) {
            sceneBrowserStatus = "A scene with that ID already exists.";
            return;
        }
        auto action = sceneActionGate.Request({scene::SceneActionKind::Create, std::move(candidate.id)},
                                              documentDirty.dirty());
        if (action) CompletePendingSceneAction(*action);
        else pendingSceneConfirmation = true;
    }

    void CompletePendingSceneAction(const scene::SceneAction& action, bool* quit = nullptr) {
        if (action.kind == scene::SceneActionKind::Quit) {
            if (quit) *quit = true;
            return;
        }
        if (action.kind == scene::SceneActionKind::OpenProject ||
            action.kind == scene::SceneActionKind::CreateProject) {
            const bool creatingProject = action.kind == scene::SceneActionKind::CreateProject;
            try {
                const fs::path projectPath = PathFromUtf8(action.target);
                if (creatingProject) {
                    const auto sources = MountedSourcePaths();
                    const auto readOnlyRoots = MountedDirectoryPaths();
                    Status createStatus;
                    if (!scene::ProjectStore::CreateProject(projectPath, sources, readOnlyRoots,
                                                            &createStatus)) {
                        projectOpenStatus = createStatus.error.starts_with("project was fully initialized")
                            ? createStatus.error
                            : "Cannot create project: " + createStatus.error;
                        return;
                    }
                }
                PreparedProject prepared;
                if (auto status = PrepareProject(projectPath, "main", prepared); !status) {
                    projectOpenStatus = creatingProject
                        ? "Project folder was initialized, but cannot be opened: " + status.error
                        : "Cannot open project: " + status.error;
                    return;
                }
                if (auto status = InstallPreparedProject(std::move(prepared), true); !status) {
                    projectOpenStatus = creatingProject
                        ? "Project folder was initialized, but cannot be opened: " + status.error
                        : "Cannot open project: " + status.error;
                    return;
                }
                if (creatingProject) sceneStatus = "Created project · loaded main scene";
                projectOpenStatus.clear();
                showProjectOpenDialog = false;
            } catch (const std::exception& error) {
                projectOpenStatus = std::string("Invalid project path: ") + error.what();
            }
            return;
        }
        const std::string& sceneId = action.target;
        if (!projectStore) return;

        scene::Document candidate;
        if (action.kind == scene::SceneActionKind::Open) {
            const auto sources = MountedSourcePaths();
            if (auto status = projectStore->Load(sceneId, candidate, sources); !status) {
                sceneBrowserStatus = "Cannot open scene " + sceneId + ": " + status.error;
                return;
            }
        } else {
            candidate.id = sceneId;
            candidate.name = sceneId;
            const auto sources = MountedSourcePaths();
            if (auto status = projectStore->Create(candidate, sources); !status) {
                const std::string failure = "Cannot create scene " + sceneId + ": " + status.error;
                RefreshProjectScenes();
                sceneBrowserStatus = failure;
                return;
            }
        }

        if (auto status = InstallDocument(std::move(candidate)); !status) {
            sceneBrowserStatus = std::string(action.kind == scene::SceneActionKind::Open ? "Cannot install scene " :
                                                                          "Scene was created but cannot be opened ") +
                                 sceneId + ": " + status.error;
            return;
        }
        sceneHistory.Clear();
        if (auto status = SetSavedBaseline(true); !status) {
            sceneBrowserStatus = "Scene is open but its saved baseline could not be recorded: " + status.error;
            return;
        }
        selectedNode.clear();
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        renameTarget.clear();
        renameSourceName.clear();
        renameBufferDirty = false;
        reparentTarget.clear();
        reparentTargetNode.clear();
        CancelPlacement("Scene changed");
        sceneStatus = std::string(action.kind == scene::SceneActionKind::Open ? "Loaded scene " : "Created scene ") + sceneId;
        selectedProjectScene = sceneId;
        std::snprintf(newSceneId, sizeof(newSceneId), "%s", "");
        RefreshProjectScenes();
        showSceneBrowser = true;
        sceneBrowserStatus.clear();
    }

    void Spawn(const std::string& path) {
        if (document.nodes.size() >= scene::kMaxSceneNodes) {
            sceneStatus = "Cannot place model: scene node limit reached";
            return;
        }
        const std::string id = NewNodeId();
        if (id.empty()) { sceneStatus = "Cannot allocate a stable scene node ID"; return; }
        scene::LocalTransform transform;
        transform.translation = {static_cast<float>(instances.size()) * 2.5f, 0, 0};
        scene::Document candidate = document;
        if (auto status = scene::AddModelInstance(candidate, id,
                path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1),
                path, std::nullopt, transform); !status) {
            sceneStatus = "Spawn refused: " + status.error;
            return;
        }
        if (!CommitDocument(std::move(candidate))) return;
        selectedNode = id;
        if (Instance* inst = FindNodeInstance(id)) {
            inst->autoPlaceOnLoad = true;
            inst->autoPlaceHistoryRevision = historyRevision;
            if (inst->model) {
                AutoPlace(id);
                if (frameOnLoad) {
                    if (const Instance* placed = FindNodeInstance(id)) FrameInstance(*placed);
                    frameOnLoad = false;
                }
            }
        }
    }

    void StartPlacement(const std::string& path) {
        placement = {};
        placement.active = true;
        placement.assetPath = path;
        placement.handle = RequestModel(path);
        if (const auto it = uploaded.find(placement.handle); it != uploaded.end()) {
            placement.gpu = it->second;
            placement.model = loader->Get(placement.handle);
        } else if (auto model = loader->Get(placement.handle)) {
            placement.model = std::move(model);
            placement.gpu = renderer->Upload(placement.model);
            if (placement.gpu) uploaded[placement.handle] = placement.gpu;
        }
        if (const scene::Node* selected = FindNode(selectedNode); selected && selected->kind == scene::NodeKind::Group)
            placement.parentId = selected->id;
        placement.status = placement.model ? "Click the viewport to place · Esc cancels" : "Loading preview · Esc cancels";
    }

    void CancelPlacement(std::string reason = "Placement canceled") {
        placement = {};
        sceneStatus = std::move(reason);
    }

    void InvalidateAssetBrowser() {
        contentCatalogLoaded = false;
        contentCatalogRejected = false;
        contentBrowserViewValid = false;
        std::vector<vfs::Entry>().swap(contentCatalog);
        contentBrowserView = {};
        contentBrowserViewDirectory.clear();
        contentBrowserViewSearch.clear();
        contentDirectory.clear();
        contentFilter[0] = '\0';
        selectedAssetPath.clear();
        if (placement.active) CancelPlacement("Mounted assets changed; placement preview canceled");
        contentBrowserStatus = "Mounted assets changed; asset selection and placement preview were cleared.";
    }

    bool BuildPlacement(float worldX, float worldY, scene::LocalTransform& local,
                        render::Float4x4& world) const {
        if (!placement.model) return false;
        std::optional<render::Float4x4> parentWorld;
        if (placement.parentId) {
            const scene::Node* parent = FindNode(*placement.parentId);
            const auto* resolvedParent = World(*placement.parentId);
            if (!parent || parent->kind != scene::NodeKind::Group || !resolvedParent) return false;
            parentWorld = *resolvedParent;
        }
        return scene::ComputeGroundPlacement(parentWorld, {worldX, worldY, 0},
                                             placement.model->boundsMin, placement.model->boundsMax,
                                             local, world).ok();
    }

    void UpdatePlacementPreview(float x, float y) {
        placement.hasPreview = false;
        if (!placement.active || !placement.model || !ui.viewport().Contains(x, y) || ui.pointerOverUi()) return;
        const auto hit = render::RayPlaneZ(camera.ScreenRay(x, y, ui.viewport()), 0);
        if (!hit || !BuildPlacement(hit->x, hit->y, placement.localTransform, placement.world)) return;
        placement.hasPreview = true;
        placement.status = "Click the viewport to place · Esc cancels";
    }

    void PlacePreview() {
        if (!placement.active || !placement.hasPreview || !placement.model) return;
        const std::string id = NewNodeId();
        if (id.empty()) { sceneStatus = "Cannot allocate a stable scene node ID"; return; }
        scene::Document candidate = document;
        if (auto status = scene::AddModelInstance(candidate, id,
                placement.assetPath.substr(placement.assetPath.find_last_of('/') == std::string::npos ? 0 : placement.assetPath.find_last_of('/') + 1),
                placement.assetPath, placement.parentId, placement.localTransform); !status) {
            sceneStatus = "Placement refused: " + status.error;
            return;
        }
        if (!CommitDocument(std::move(candidate))) return;
        selectedNode = id;
        placement = {};
        sceneStatus = "Model placed · unsaved";
    }

    void AutoPlace(std::string_view nodeId) {
        const scene::Node* node = FindNode(nodeId);
        Instance* inst = FindNodeInstance(nodeId);
        if (!node || !inst || !inst->model || node->parentId || node->localTransform.translation.z != 0) return;
        scene::Document candidate = document;
        for (auto& item : candidate.nodes)
            if (item.id == nodeId) item.localTransform.translation.z = -inst->model->boundsMin[2] * item.localTransform.scale.z;
        if (candidate == document) {
            inst->autoPlaceOnLoad = false;
            return;
        }
        const bool recordAsIndependentEdit = inst->autoPlaceHistoryRevision != historyRevision;
        if (auto status = InstallDocument(std::move(candidate), recordAsIndependentEdit); !status) {
            sceneStatus = "Auto placement refused: " + status.error;
            return;
        }
        if (recordAsIndependentEdit) ++historyRevision;
        SetHistoryStatus("Auto placement");
        if (Instance* updated = FindNodeInstance(nodeId)) updated->autoPlaceOnLoad = false;
    }

    Status PrepareProject(const fs::path& root, std::string_view sceneId, PreparedProject& prepared) const {
        std::error_code ec;
        const fs::path canonicalRoot = fs::canonical(root, ec);
        if (ec) return Status::Fail("project directory cannot be resolved: " + ec.message());
        if (!fs::is_directory(canonicalRoot, ec) || ec)
            return Status::Fail("project root must be an existing directory");

        const auto sourcePaths = MountedSourcePaths();
        if (auto status = scene::ProjectStore::ValidateSourceSeparation(canonicalRoot, sourcePaths); !status)
            return status;

        const auto readOnlyRoots = MountedDirectoryPaths();

        Status status;
        auto store = scene::ProjectStore::Open(canonicalRoot, readOnlyRoots, &status);
        if (!store) return status;

        scene::Document initial;
        initial.id = std::string(sceneId);
        initial.name = "Untitled";
        if (auto valid = scene::Validate(initial); !valid) return valid;
        const fs::path scenePath = canonicalRoot / (initial.id + ".scene.json");
        const auto fileStatus = fs::symlink_status(scenePath, ec);
        bool saved = false;
        if (!ec && (fs::exists(fileStatus) || fs::is_symlink(fileStatus))) {
            if (auto loaded = store->Load(sceneId, initial, sourcePaths); !loaded) return loaded;
            saved = true;
        } else if (ec && ec != std::errc::no_such_file_or_directory) {
            return Status::Fail("cannot inspect project scene file: " + ec.message());
        }
        std::vector<scene::ResolvedTransform> worldTransforms;
        if (auto resolved = scene::ResolveWorldTransforms(initial, worldTransforms); !resolved)
            return Status::Fail("project scene cannot be installed: " + resolved.error);
        scene::DocumentDirtyState dirty;
        if (auto baseline = dirty.SetBaseline(initial, saved); !baseline) return baseline;

        prepared.store = std::move(*store);
        prepared.root = canonicalRoot;
        prepared.document = std::move(initial);
        prepared.dirty = std::move(dirty);
        prepared.saved = saved;
        return Status::Ok();
    }

    Status InstallPreparedProject(PreparedProject prepared, bool showBrowser) {
        if (!prepared.store) return Status::Fail("project was not prepared");
        const bool saved = prepared.saved;
        if (auto installed = InstallDocument(std::move(prepared.document)); !installed) return installed;
        projectStore = std::move(prepared.store);
        projectRoot = std::move(prepared.root);
        documentDirty = std::move(prepared.dirty);
        sceneHistory.Clear();
        selectedNode.clear();
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        inspectorRotationNode.clear();
        inspectorRotationInitialized = false;
        inspectorRotationActive = false;
        renameTarget.clear();
        renameSourceName.clear();
        renameBufferDirty = false;
        reparentTarget.clear();
        reparentTargetNode.clear();
        CancelPlacement("Project changed");
        frameOnLoad = true;
        sceneStatus = saved ? "Opened project · loaded scene " + document.id
                            : "Opened project · main scene is new and unsaved";
        selectedProjectScene = document.id;
        if (showBrowser) {
            RefreshProjectScenes();
            showSceneBrowser = true;
        }
        return Status::Ok();
    }

    Status OpenProject(const fs::path& root, std::string_view sceneId, bool showBrowser = false) {
        PreparedProject prepared;
        if (auto status = PrepareProject(root, sceneId, prepared); !status) return status;
        return InstallPreparedProject(std::move(prepared), showBrowser);
    }

    Status InitializeEmptyScene() {
        document = {};
        document.id = "untitled";
        document.name = "Untitled";
        if (auto status = InstallDocument(document); !status) return status;
        sceneHistory.Clear();
        if (auto baseline = SetSavedBaseline(true); !baseline) return baseline;
        sceneStatus = "In-memory scene · open a project from File or pass --project DIR to save";
        return Status::Ok();
    }

    void ClearScene() {
        scene::Document candidate = document;
        candidate.nodes.clear();
        if (CommitDocument(std::move(candidate))) {
            selectedNode.clear();
            CancelPlacement("Scene cleared · unsaved");
        }
    }

    void RemoveSelected() {
        if (selectedNode.empty()) return;
        scene::Document candidate = document;
        if (auto status = scene::RemoveSubtree(candidate, selectedNode); !status) {
            sceneStatus = "Delete refused: " + status.error;
            return;
        }
        if (CommitDocument(std::move(candidate))) {
            selectedNode.clear();
            sceneStatus = "Node removed · unsaved";
        }
    }

    // Finished loads: upload once per model, attach to document instances and the active preview.
    void PumpLoader() {
        for (asset::ModelHandle h : loader->TakeCompleted()) {
            const auto info = loader->Info(h);
            auto model = loader->Get(h);
            if (!model) {
                log::Error("assets", info.path + ": " + info.error);
                if (placement.active && placement.handle == h) placement.status = "Preview load failed: " + info.error;
                continue;
            }
            const bool needed = (placement.active && placement.handle == h) ||
                                std::any_of(instances.begin(), instances.end(),
                                            [&](const Instance& inst) { return inst.handle == h; });
            if (!needed) continue;
            const render::GpuModel gpu = renderer->Upload(model);
            uploaded[h] = gpu;
            std::vector<std::string> autoPlace;
            std::string firstLoadedNode;
            for (auto& inst : instances)
                if (inst.handle == h) {
                    inst.gpu = gpu;
                    inst.model = model;
                    if (firstLoadedNode.empty()) firstLoadedNode = inst.nodeId;
                    if (inst.autoPlaceOnLoad) autoPlace.push_back(inst.nodeId);
                }
            if (placement.active && placement.handle == h) {
                placement.gpu = gpu;
                placement.model = model;
                placement.status = "Click the viewport to place · Esc cancels";
            }
            for (const auto& id : autoPlace) AutoPlace(id);
            if (frameOnLoad && !firstLoadedNode.empty()) {
                if (const Instance* firstLoaded = FindNodeInstance(firstLoadedNode)) FrameInstance(*firstLoaded);
                frameOnLoad = false;
            }
        }
        for (auto& inst : instances)
            if (!inst.gpu)
                if (auto it = uploaded.find(inst.handle); it != uploaded.end()) {
                    inst.gpu = it->second;
                    inst.model = loader->Get(inst.handle);
                }
        if (placement.active && !placement.gpu)
            if (auto it = uploaded.find(placement.handle); it != uploaded.end()) {
                placement.gpu = it->second;
                placement.model = loader->Get(placement.handle);
            }
    }

    // Runs a scenario file on the game thread. The dock and --scenario both come through here, so
    // the UI and the smoke path cannot disagree about what a scenario did.
    void RunScenarioFile(const std::string& path) {
        scenarioResults.clear();
        scenarioPassed = false;
        hostd::Json scenario;
        std::string error;
        if (!hostd::LoadScenario(path, scenario, error)) {
            scenarioStatus = "load failed: " + error;
            log::Error("scenario", path + ": " + error);
            return;
        }
        scenarioPassed = hostd::RunScenario(scenario, scenarioResults);
        size_t matched = 0;
        for (const auto& r : scenarioResults)
            if (r.matched) ++matched;
        scenarioStatus = (scenarioPassed ? "pass " : "FAIL ") + std::to_string(matched) + "/" +
                         std::to_string(scenarioResults.size()) + " step(s)";
        if (!scenarioPassed) log::Error("scenario", path + ": " + scenarioStatus);
        else log::Info("scenario", path + ": " + scenarioStatus);
    }

    void FrameInstance(const Instance& inst) {
        if (!inst.model) return;
        std::array<float, 3> mn{}, mx{};
        if (render::TransformBounds(inst.model->boundsMin, inst.model->boundsMax, inst.world, mn, mx))
            camera.Frame(mn, mx);
    }

    std::vector<render::PickTarget> PickTargets() const {
        std::vector<render::PickTarget> targets;
        for (const auto& inst : instances)
            if (inst.model) targets.push_back({inst.id, inst.model.get(), inst.world});
        return targets;
    }

    void DrawUi(bool& quit);
    void DrawTransformGizmo() const;
    void DrawMenu(bool& quit);
    void DrawContent();
    void DrawMounts();
    void DrawScene();
    void DrawSceneBrowser();
    void DrawProjectOpenDialog();
    void DrawSceneChangeConfirmation(bool& quit);
    void DrawInspector();
    void DrawPlugins();
    void DrawLoader();
    void DrawStats();
    void DrawLog();
    void DrawScenario();
    void DrawProduct();
    void DrawSurface();
};

// ---- input layers --------------------------------------------------------------------------------

class UiBlocker final : public ui::InputLayer {
public:
    explicit UiBlocker(Editor& e) : e_(e) {}
    const char* Name() const override { return "ui"; }
    ui::Reply OnInput(const ui::InputEvent& ev) override {
        if (ev.kind == ui::InputKind::KeyDown) {
            if ((e_.placement.active || e_.transformGizmoGestureActive) &&
                ev.key == ImGuiKey_Escape)
                return ui::Reply::Unhandled();
            return e_.ui.keyboardToUi() ? ui::Reply::Handled() : ui::Reply::Unhandled();
        }
        if (e_.ui.pointerOverUi() || !e_.ui.viewport().Contains(ev.x, ev.y)) return ui::Reply::Handled();
        return ui::Reply::Unhandled();
    }

private:
    Editor& e_;
};

class PlacementTool final : public ui::InputLayer {
public:
    explicit PlacementTool(Editor& e) : e_(e) {}
    const char* Name() const override { return "placement"; }
    ui::Reply OnInput(const ui::InputEvent& ev) override {
        if (!e_.placement.active) return ui::Reply::Unhandled();
        if (ev.kind == ui::InputKind::KeyDown && ev.key == ImGuiKey_Escape) {
            e_.CancelPlacement();
            return ui::Reply::Handled();
        }
        if (ev.kind == ui::InputKind::PointerMove) {
            e_.UpdatePlacementPreview(ev.x, ev.y);
            return ui::Reply::Handled();
        }
        if (ev.kind == ui::InputKind::PointerDown && ev.button == ui::PointerButton::Left) {
            e_.UpdatePlacementPreview(ev.x, ev.y);
            if (e_.placement.hasPreview) e_.PlacePreview();
            else if (!e_.placement.model) e_.placement.status = "Preview still loading · Esc cancels";
            else e_.placement.status = "Move into the viewport to preview · Esc cancels";
            return ui::Reply::Capture();
        }
        return ui::Reply::Unhandled();
    }

private:
    Editor& e_;
};

// Handles world-axis translation, local-axis scale, and local-axis rotation. Each gesture edits
// only the selected node's TRS and is committed as one interactive history transaction.
class AxisTransformTool final : public ui::InputLayer {
public:
    explicit AxisTransformTool(Editor& editor) : editor_(editor) {}
    const char* Name() const override { return "transform-gizmo"; }

    ui::Reply OnInput(const ui::InputEvent& event) override {
        // Placement owns viewport clicks while a preview is active. The gizmo is hidden in this
        // state so the visible affordance always matches the winning input layer.
        if (editor_.placement.active && event.kind == ui::InputKind::PointerDown)
            return ui::Reply::Unhandled();
        if (event.kind == ui::InputKind::PointerCancel) {
            const bool keptPartialEdit = dragging_ && undoCaptured_;
            ResetGesture();
            if (keptPartialEdit)
                editor_.sceneStatus = "Transform drag interrupted; last valid edit kept · undo available";
            return ui::Reply::Handled();
        }
        if (event.kind == ui::InputKind::KeyDown && gestureCaptured_) {
            if (event.key == ImGuiKey_Escape) {
                CancelGesture();
                return ui::Reply::Release();
            }
            editor_.sceneStatus = "Finish or cancel the active transform drag before using editor shortcuts";
            return ui::Reply::Handled();
        }
        if (event.kind == ui::InputKind::PointerUp && event.button == ui::PointerButton::Left) {
            if (!gestureCaptured_) return ui::Reply::Unhandled();
            ResetGesture();
            return ui::Reply::Release();
        }
        if (event.kind == ui::InputKind::PointerDown && event.button == ui::PointerButton::Left) {
            if (event.ctrl) return ui::Reply::Unhandled();  // Keep Ctrl+drag's legacy ground-plane move.
            TransformGizmoGeometry geometry;
            if (!editor_.BuildGizmoGeometry(geometry)) return ui::Reply::Unhandled();
            int axisIndex = -1;
            const auto gestureMode = editor_.gizmoMode;
            if (gestureMode == TransformGizmoMode::Rotate) {
                const int ringSegment = render::HitTestScreenSegments(event.x, event.y,
                    geometry.rotationHandles, 9.0f, 1.5f);
                if (ringSegment >= 0)
                    axisIndex = ringSegment / static_cast<int>(render::kRotationRingSegments);
            } else {
                axisIndex = render::HitTestScreenSegments(event.x, event.y, geometry.hitHandles);
            }
            if (axisIndex < 0) return ui::Reply::Unhandled();
            const scene::Node* selected = editor_.FindNode(editor_.selectedNode);
            const auto* world = selected ? editor_.World(selected->id) : nullptr;
            if (!selected || !world) return ui::Reply::Unhandled();
            if (!render::ValidateGizmoSnapSettings(editor_.gizmoSnapSettings)) {
                editor_.sceneStatus = "Transform snap settings are invalid; drag refused";
                return ui::Reply::Handled();
            }
            snapSettings_ = editor_.gizmoSnapSettings;
            gestureCaptured_ = true;
            editor_.transformGizmoGestureActive = true;
            mode_ = gestureMode;
            axisIndex_ = axisIndex;

            startWorldPosition_ = TransformPoint({0, 0, 0}, *world);
            const auto startRay = editor_.camera.ScreenRay(event.x, event.y, editor_.ui.viewport());
            if (mode_ == TransformGizmoMode::Rotate) {
                rotationBasisU_ = geometry.rotationBasisU[axisIndex];
                rotationBasisV_ = geometry.rotationBasisV[axisIndex];
                render::Float3 hit;
                if (!render::IntersectRayPlane(startRay, geometry.worldCenter,
                                               rotationBasisU_, rotationBasisV_, hit) ||
                    !render::PlaneBasisCoordinates(geometry.worldCenter, rotationBasisU_,
                                                   rotationBasisV_, hit,
                                                   rotationLastU_, rotationLastV_)) {
                    editor_.sceneStatus = "Rotation ring unavailable from this view direction";
                    return ui::Reply::Capture();
                }
                rotationStart_ = selected->localTransform.rotation;
                rotationAccumulated_ = 0;
            } else {
                axis_ = geometry.directions[axisIndex];
                scaleHandleWorldLength_ = geometry.worldLength;
                startScale_ = axisIndex == 0 ? selected->localTransform.scale.x :
                              axisIndex == 1 ? selected->localTransform.scale.y : selected->localTransform.scale.z;
                if (!render::ClosestRayAxisParameter(startRay, startWorldPosition_, axis_, startParameter_)) {
                    editor_.sceneStatus = "Transform axis unavailable from this view direction";
                    return ui::Reply::Capture();
                }
                if (mode_ == TransformGizmoMode::Move && selected->parentId) {
                    const auto* parentWorld = editor_.World(*selected->parentId);
                    if (!parentWorld || !render::InvertAffine(*parentWorld, parentInverse_)) {
                        editor_.sceneStatus = "Axis translation unavailable: parent transform cannot be inverted";
                        return ui::Reply::Capture();
                    }
                } else {
                    parentInverse_ = {};
                }
            }
            draggedNode_ = selected->id;
            undoCaptured_ = false;
            gestureStartHistoryRevision_ = editor_.historyRevision;
            dragging_ = true;
            editor_.transformGizmoDragging = true;
            editor_.transformGizmoAxis = axisIndex;
            return ui::Reply::Capture();
        }
        if (event.kind == ui::InputKind::PointerMove && dragging_ && event.leftHeld) {
            if (editor_.placement.active) {
                StopGesture("Transform drag stopped because asset placement became active");
                return ui::Reply::Release();
            }
            if (editor_.gizmoMode != mode_) {
                StopGesture("Transform drag stopped because gizmo mode changed");
                return ui::Reply::Release();
            }
            if (editor_.selectedNode != draggedNode_ || !editor_.FindNode(draggedNode_)) {
                StopGesture("Transform drag stopped because its selection changed");
                return ui::Reply::Release();
            }
            const uint64_t expectedRevision = undoCaptured_ ? dragHistoryRevision_ : gestureStartHistoryRevision_;
            if (editor_.historyRevision != expectedRevision) {
                StopGesture("Transform drag stopped because another scene edit completed");
                return ui::Reply::Release();
            }
            const auto ray = editor_.camera.ScreenRay(event.x, event.y, editor_.ui.viewport());
            scene::Document candidate = editor_.document;
            const auto node = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [&](const auto& item) {
                return item.id == draggedNode_;
            });
            if (node == candidate.nodes.end()) return ui::Reply::Handled();
            if (mode_ == TransformGizmoMode::Rotate) {
                render::Float3 hit;
                float coordinateU = 0, coordinateV = 0, stepRadians = 0;
                if (!render::IntersectRayPlane(ray, startWorldPosition_, rotationBasisU_,
                                               rotationBasisV_, hit) ||
                    !render::PlaneBasisCoordinates(startWorldPosition_, rotationBasisU_,
                                                   rotationBasisV_, hit, coordinateU, coordinateV) ||
                    !render::SignedPlanarAngleDelta(rotationLastU_, rotationLastV_,
                                                    coordinateU, coordinateV, stepRadians))
                    return ui::Reply::Handled();
                constexpr double kMaxDragAngleRadians = 200.0 * 3.14159265358979323846;
                const double nextAngle = rotationAccumulated_ + stepRadians;
                if (!std::isfinite(nextAngle) || std::fabs(nextAngle) > kMaxDragAngleRadians) {
                    editor_.sceneStatus = "Rotation drag reached the 100-turn gesture limit";
                    return ui::Reply::Handled();
                }
                scene::Quaternion rotation;
                float appliedAngle = static_cast<float>(nextAngle);
                if (snapSettings_.rotationEnabled &&
                    !render::SnapRotationDelta(appliedAngle, snapSettings_.rotationStepDegrees, appliedAngle)) {
                    editor_.sceneStatus = "Rotation snap refused: step or angle is invalid";
                    return ui::Reply::Handled();
                }
                if (auto status = scene::RotateLocalAxis(rotationStart_, static_cast<size_t>(axisIndex_),
                                                         appliedAngle, rotation); !status) {
                    editor_.sceneStatus = "Rotation refused: " + status.error;
                    return ui::Reply::Handled();
                }
                node->localTransform.rotation = rotation;
                if (!undoCaptured_) historyBeforeDocument_ = editor_.document;
                if (auto status = editor_.CommitInteractiveDocument(std::move(candidate), undoCaptured_); !status)
                    return ui::Reply::Handled();
                rotationAccumulated_ = nextAngle;
                rotationLastU_ = coordinateU;
                rotationLastV_ = coordinateV;
                if (undoCaptured_) dragHistoryRevision_ = editor_.historyRevision;
                return ui::Reply::Handled();
            }

            float currentParameter = 0;
            if (!render::ClosestRayAxisParameter(ray, startWorldPosition_, axis_, currentParameter))
                return ui::Reply::Handled();
            if (mode_ == TransformGizmoMode::Scale) {
                if (!std::isfinite(scaleHandleWorldLength_) || scaleHandleWorldLength_ <= 0) return ui::Reply::Handled();
                float desiredScale = 0;
                if (!render::AxisScaleFromParameter(startScale_, startParameter_, currentParameter,
                                                    scaleHandleWorldLength_, desiredScale,
                                                    snapSettings_.scaleEnabled ? snapSettings_.scaleStep : 0.0f))
                    return ui::Reply::Handled();
                if (auto status = scene::SetLocalScaleAxis(node->localTransform,
                                                           static_cast<size_t>(axisIndex_), desiredScale); !status) {
                    editor_.sceneStatus = "Scale refused: " + status.error;
                    return ui::Reply::Handled();
                }
            } else {
                render::Float3 desiredWorld;
                if (!render::AxisTranslationFromParameter(startWorldPosition_, axis_, startParameter_,
                                                          currentParameter, desiredWorld,
                                                          snapSettings_.translationEnabled
                                                              ? snapSettings_.translationStep : 0.0f))
                    return ui::Reply::Handled();
                const auto localTranslation = TransformPoint(desiredWorld, parentInverse_);
                if (!std::isfinite(localTranslation.x) || !std::isfinite(localTranslation.y) ||
                    !std::isfinite(localTranslation.z))
                    return ui::Reply::Handled();
                node->localTransform.translation = {localTranslation.x, localTranslation.y, localTranslation.z};
            }
            if (!undoCaptured_) historyBeforeDocument_ = editor_.document;
            editor_.CommitInteractiveDocument(std::move(candidate), undoCaptured_);
            if (undoCaptured_) dragHistoryRevision_ = editor_.historyRevision;
            return ui::Reply::Handled();
        }
        return ui::Reply::Unhandled();
    }

private:
    void StopGesture(std::string_view reason) {
        const bool keptPartialEdit = undoCaptured_;
        ResetGesture();
        editor_.sceneStatus = std::string(reason) +
            (keptPartialEdit ? "; last valid transform kept · undo available" : "; no transform edit applied");
    }

    void ResetGesture() {
        gestureCaptured_ = false;
        dragging_ = false;
        undoCaptured_ = false;
        draggedNode_.clear();
        historyBeforeDocument_ = {};
        gestureStartHistoryRevision_ = 0;
        dragHistoryRevision_ = 0;
        mode_ = TransformGizmoMode::Move;
        axisIndex_ = -1;
        rotationBasisU_ = {};
        rotationBasisV_ = {};
        rotationStart_ = {};
        rotationLastU_ = rotationLastV_ = 0;
        rotationAccumulated_ = 0;
        snapSettings_ = {};
        editor_.transformGizmoDragging = false;
        editor_.transformGizmoGestureActive = false;
        editor_.transformGizmoAxis = -1;
    }

    void CancelGesture() {
        if (dragging_ && undoCaptured_) {
            scene::Document restored;
            if (auto status = editor_.sceneHistory.CancelLatestChange(historyBeforeDocument_, restored); !status) {
                editor_.sceneStatus = "Transform drag could not be canceled safely: " + status.error +
                                      "; current transform kept";
                ResetGesture();
                return;
            }
            editor_.inspectorUndoCaptured = false;
            editor_.inspectorUndoNode.clear();
            if (auto status = editor_.InstallDocument(std::move(restored)); !status) {
                editor_.sceneStatus = "Transform drag cancel could not restore its starting scene: " + status.error;
                ResetGesture();
                return;
            }
            ++editor_.historyRevision;
            ResetGesture();
            editor_.SetHistoryStatus("Drag canceled");
            return;
        }
        ResetGesture();
        editor_.sceneStatus = "Transform drag canceled";
    }

    Editor& editor_;
    render::Float3 axis_{};
    render::Float3 startWorldPosition_{};
    float startParameter_ = 0;
    render::Float4x4 parentInverse_{};
    std::string draggedNode_;
    scene::Document historyBeforeDocument_;
    uint64_t gestureStartHistoryRevision_ = 0;
    uint64_t dragHistoryRevision_ = 0;
    float startScale_ = 1;
    float scaleHandleWorldLength_ = 0;
    render::Float3 rotationBasisU_{};
    render::Float3 rotationBasisV_{};
    scene::Quaternion rotationStart_{};
    float rotationLastU_ = 0;
    float rotationLastV_ = 0;
    double rotationAccumulated_ = 0;
    render::GizmoSnapSettings snapSettings_;
    int axisIndex_ = -1;
    TransformGizmoMode mode_ = TransformGizmoMode::Move;
    bool gestureCaptured_ = false;
    bool dragging_ = false;
    bool undoCaptured_ = false;
};

// Ctrl + left drag moves the selection across its ground plane.
class MoveTool final : public ui::InputLayer {
public:
    explicit MoveTool(Editor& e) : e_(e) {}
    const char* Name() const override { return "move"; }
    ui::Reply OnInput(const ui::InputEvent& ev) override {
        if (ev.kind == ui::InputKind::PointerCancel) {
            dragging_ = false;
            undoCaptured_ = false;
            draggedNode_.clear();
            return ui::Reply::Handled();
        }
        if (ev.kind == ui::InputKind::PointerUp && ev.button == ui::PointerButton::Left) {
            const bool wasDragging = dragging_;
            dragging_ = false;
            undoCaptured_ = false;
            draggedNode_.clear();
            return wasDragging ? ui::Reply::Release() : ui::Reply::Unhandled();
        }
        if (ev.kind == ui::InputKind::PointerDown && ev.button == ui::PointerButton::Left && ev.ctrl) {
            const scene::Node* selected = e_.FindNode(e_.selectedNode);
            const render::Float4x4* world = selected ? e_.World(selected->id) : nullptr;
            if (!selected || !world) return ui::Reply::Unhandled();
            const auto origin = TransformPoint({0, 0, 0}, *world);
            const auto hit = render::RayPlaneZ(e_.camera.ScreenRay(ev.x, ev.y, e_.ui.viewport()), origin.z);
            if (!hit) return ui::Reply::Unhandled();
            draggedNode_ = selected->id;
            dragPlaneZ_ = origin.z;
            grab_ = {hit->x - origin.x, hit->y - origin.y, 0};
            undoCaptured_ = false;
            dragging_ = true;
            return ui::Reply::Capture();
        }
        if (ev.kind == ui::InputKind::PointerMove && dragging_ && ev.leftHeld) {
            const scene::Node* selected = e_.FindNode(draggedNode_);
            const render::Float4x4* world = selected ? e_.World(selected->id) : nullptr;
            if (!selected || !world) return ui::Reply::Handled();
            const auto hit = render::RayPlaneZ(e_.camera.ScreenRay(ev.x, ev.y, e_.ui.viewport()), dragPlaneZ_);
            if (!hit) return ui::Reply::Handled();
            render::Float4x4 parentInverse;
            if (selected->parentId) {
                const auto* parent = e_.World(*selected->parentId);
                if (!parent || !render::InvertAffine(*parent, parentInverse)) return ui::Reply::Handled();
            }
            const auto local = TransformPoint({hit->x - grab_.x, hit->y - grab_.y, dragPlaneZ_}, parentInverse);
            scene::Document candidate = e_.document;
            for (auto& node : candidate.nodes)
                if (node.id == selected->id) node.localTransform.translation = {local.x, local.y, local.z};
            e_.CommitInteractiveDocument(std::move(candidate), undoCaptured_);
            return ui::Reply::Handled();
        }
        return ui::Reply::Unhandled();
    }

private:
    Editor& e_;
    render::Float3 grab_;
    float dragPlaneZ_ = 0;
    std::string draggedNode_;
    bool dragging_ = false;
    bool undoCaptured_ = false;
};

// Right drag orbits, middle drag pans, wheel dollies, left click picks, F frames, Del removes.
class CameraTool final : public ui::InputLayer {
public:
    explicit CameraTool(Editor& e) : e_(e) {}
    const char* Name() const override { return "camera"; }
    ui::Reply OnInput(const ui::InputEvent& ev) override {
        switch (ev.kind) {
        case ui::InputKind::PointerDown:
            if (ev.button == ui::PointerButton::Right || ev.button == ui::PointerButton::Middle) return ui::Reply::Capture();
            if (ev.button == ui::PointerButton::Left) {
                downX_ = ev.x;
                downY_ = ev.y;
                return ui::Reply::Capture();
            }
            break;
        case ui::InputKind::PointerMove:
            if (ev.rightHeld) e_.camera.Orbit(ev.dx, ev.dy);
            else if (ev.middleHeld) e_.camera.Pan(ev.dx, ev.dy, e_.ui.viewport().h);
            else return ui::Reply::Unhandled();
            return ui::Reply::Handled();
        case ui::InputKind::PointerUp:
            if (ev.button == ui::PointerButton::Left && std::fabs(ev.x - downX_) < 4 && std::fabs(ev.y - downY_) < 4) {
                const auto targets = e_.PickTargets();
                const auto hit = render::Pick(e_.camera.ScreenRay(ev.x, ev.y, e_.ui.viewport()), targets);
                const Instance* instance = hit ? e_.Find(hit->id) : nullptr;
                e_.selectedNode = instance ? instance->nodeId : std::string{};
            }
            return ui::Reply::Release();
        case ui::InputKind::PointerCancel:
            return ui::Reply::Handled();
        case ui::InputKind::Wheel:
            e_.camera.Dolly(ev.wheel);
            return ui::Reply::Handled();
        case ui::InputKind::KeyDown:
            if (ev.key == ImGuiKey_Z && ev.ctrl) { e_.UndoScene(); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Y && ev.ctrl) { e_.RedoScene(); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_D && ev.ctrl && !e_.selectedNode.empty()) {
                e_.DuplicateSelected();
                return ui::Reply::Handled();
            }
            if (ev.key == ImGuiKey_S && ev.ctrl) { e_.SaveScene(); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_F) { if (auto* s = e_.FindNodeInstance(e_.selectedNode)) e_.FrameInstance(*s); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Delete && !e_.selectedNode.empty()) { e_.RemoveSelected(); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Escape) { e_.selectedNode.clear(); return ui::Reply::Handled(); }
            break;
        }
        return ui::Reply::Unhandled();
    }

private:
    Editor& e_;
    float downX_ = 0, downY_ = 0;
};

// ---- panels --------------------------------------------------------------------------------------

void Editor::DrawMenu(bool& quit) {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, sceneHistory.CanUndo())) UndoScene();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, sceneHistory.CanRedo())) RedoScene();
        ImGui::Separator();
        if (ImGui::MenuItem("Open project...")) OpenProjectDialog();
        if (ImGui::MenuItem("Create project...")) CreateProjectDialog();
        if (ImGui::MenuItem("Project scenes...", nullptr, false, projectStore.has_value())) OpenSceneBrowser();
        if (ImGui::MenuItem("Save scene", "Ctrl+S", false, projectStore.has_value())) SaveScene();
        if (ImGui::MenuItem("Clear scene")) ClearScene();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) RequestQuit(quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Content", nullptr, &showContent);
        ImGui::MenuItem("Mounts", nullptr, &showMounts);
        ImGui::MenuItem("Scene", nullptr, &showScene);
        ImGui::MenuItem("Inspector", nullptr, &showInspector);
        ImGui::MenuItem("Plugins", nullptr, &showPlugins);
        ImGui::MenuItem("Asset loader", nullptr, &showLoader);
        ImGui::MenuItem("Stats", nullptr, &showStats);
        ImGui::MenuItem("Log", nullptr, &showLog);
        ImGui::MenuItem("Scenario", nullptr, &showScenario);
        ImGui::MenuItem("Product", nullptr, &showProduct);
        ImGui::MenuItem("Plugin surface", nullptr, &showSurface);
        ImGui::MenuItem("Designer", nullptr, &designer.open);
        ImGui::MenuItem("NetSandbox", nullptr, &showNetSandbox);
        ImGui::Separator();
        ImGui::MenuItem("ImGui demo", nullptr, &showDemo);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("scuba-engine %s on sco-core SDK %s", SCUBA_VERSION, ScoHost::SdkVersion());
        ImGui::TextDisabled("RMB orbit  MMB pan  wheel zoom  LMB select  Ctrl+LMB move  F frame  Del remove");
        ImGui::TextDisabled("Mock game data only: nothing here has run in Star Citizen.");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Editor::DrawContent() {
    if (!ImGui::Begin("Content", &showContent)) { ImGui::End(); return; }
    // VFS directory mounts are indexed when mounted. Read a stable mount-stack snapshot in bounded
    // pages; edits on disk appear after the directory mount is remounted.
    if (!contentCatalogLoaded) {
        contentCatalogLoaded = true;
        contentBrowserViewValid = false;
        contentCatalogRejected = false;
        contentBrowserStatus.clear();
        contentCatalog.clear();
        uint64_t snapshotId = 0;
        std::string after;
        while (contentCatalog.size() < asset::kMaxBrowserCatalogEntries) {
            const size_t remaining = asset::kMaxBrowserCatalogEntries - contentCatalog.size();
            const size_t limit = std::min(remaining, vfs::kMaxVfsListPageEntries);
            vfs::VfsListPage page;
            if (auto status = vfs.ListPage("", after, limit, snapshotId, page); !status) {
                contentCatalogRejected = true;
                contentBrowserStatus = "Asset browser could not read a stable VFS listing: " + status.error;
                break;
            }
            if (snapshotId == 0) snapshotId = page.snapshotId;
            if (snapshotId == 0 || page.entries.size() > limit || page.snapshotId != snapshotId ||
                (page.entries.empty() ? !page.nextCursor.empty() : page.nextCursor != page.entries.back().path) ||
                (page.truncated && (page.entries.empty() || page.nextCursor.empty() || page.nextCursor == after))) {
                contentCatalogRejected = true;
                contentBrowserStatus = "Asset browser stopped: the VFS returned an invalid listing page.";
                break;
            }
            contentCatalog.insert(contentCatalog.end(), page.entries.begin(), page.entries.end());
            if (!page.truncated) break;
            if (contentCatalog.size() == asset::kMaxBrowserCatalogEntries) {
                contentCatalogRejected = true;
                contentBrowserStatus = "Asset browser stopped: the mounted VFS has more than 65,536 files. Mount a narrower directory or container.";
                break;
            }
            after = page.nextCursor;
        }
        if (contentCatalogRejected) {
            std::vector<vfs::Entry>().swap(contentCatalog);
        }
    }

    if (!contentCatalogRejected && (!contentBrowserViewValid || contentBrowserViewDirectory != contentDirectory ||
                                    contentBrowserViewSearch != contentFilter)) {
        if (auto status = asset::BuildBrowserContents(contentCatalog, contentDirectory, contentFilter,
                                                       contentBrowserView); !status) {
            contentBrowserStatus = "Asset browser stopped: " + status.error;
            contentCatalogRejected = true;
            std::vector<vfs::Entry>().swap(contentCatalog);
            contentDirectory.clear();
            contentFilter[0] = '\0';
            selectedAssetPath.clear();
            if (placement.active) CancelPlacement("Asset browser rejected the mounted catalogue");
            contentBrowserView = {};
        }
        contentBrowserViewDirectory = contentDirectory;
        contentBrowserViewSearch = contentFilter;
        contentBrowserViewValid = true;
    }
    const auto& browser = contentBrowserView;

    if (ImGui::SmallButton("All assets")) {
        contentDirectory.clear();
        contentFilter[0] = '\0';
        selectedAssetPath.clear();
        if (!contentCatalogRejected) contentBrowserStatus.clear();
    }
    std::string prefix;
    size_t start = 0;
    while (start < contentDirectory.size()) {
        const size_t slash = contentDirectory.find('/', start);
        const size_t end = slash == std::string::npos ? contentDirectory.size() : slash;
        const std::string segment = contentDirectory.substr(start, end - start);
        prefix = prefix.empty() ? segment : prefix + "/" + segment;
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        ImGui::PushID(prefix.c_str());
        if (ImGui::SmallButton(segment.c_str())) {
            contentDirectory = prefix;
            contentFilter[0] = '\0';
            selectedAssetPath.clear();
            if (!contentCatalogRejected) contentBrowserStatus.clear();
        }
        ImGui::PopID();
        start = end + 1;
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "search files in this folder and below", contentFilter,
                             sizeof contentFilter);
    if (ImGui::RadioButton("Tiles##asset-view", contentTileView)) contentTileView = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("List##asset-view", !contentTileView)) contentTileView = false;

    auto drawList = [&](size_t count, auto drawItem) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(count));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                drawItem(static_cast<size_t>(index));
        }
    };
    auto drawTileBackdrop = [&](const ImVec2& position, float width, float height) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            position, ImVec2(position.x + width, position.y + height),
            ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    };
    auto drawTileBorder = [&](const ImVec2& position, float width, float height) {
        ImGui::GetWindowDrawList()->AddRect(
            position, ImVec2(position.x + width, position.y + height),
            ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
    };
    auto drawTiles = [&](size_t count, float tileHeight, auto drawTile) {
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float spacingX = ImGui::GetStyle().ItemSpacing.x;
        const float tileWidth = std::max(72.0f, std::min(156.0f, availableWidth));
        const int columns = std::max(1, static_cast<int>((availableWidth + spacingX) / (tileWidth + spacingX)));
        const int rows = static_cast<int>((count + static_cast<size_t>(columns) - 1) /
                                          static_cast<size_t>(columns));
        ImGuiListClipper clipper;
        clipper.Begin(rows, tileHeight + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const size_t index = static_cast<size_t>(row) * static_cast<size_t>(columns) +
                                         static_cast<size_t>(column);
                    if (index >= count) break;
                    if (column != 0) ImGui::SameLine();
                    drawTile(index, tileWidth, tileHeight);
                }
            }
        }
    };

    ImGui::TextDisabled("%s", contentFilter[0] ? "Matching folders" : "Folders");
    if (ImGui::BeginChild("folders", ImVec2(0, contentTileView ? 116.0f : 92.0f), true)) {
        auto drawFolder = [&](size_t index, float width, float height) {
            const auto& folder = browser.directories[index];
            ImGui::PushID(folder.path.c_str());
            const std::string label = contentTileView ? "[FOLDER]\n" + folder.name + "/" : folder.name + "/";
            const ImVec2 size = contentTileView ? ImVec2(width, height) : ImVec2(0, 0);
            const ImVec2 tilePosition = ImGui::GetCursorScreenPos();
            if (contentTileView) drawTileBackdrop(tilePosition, width, height);
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, size)) {
                contentDirectory = folder.path;
                contentFilter[0] = '\0';
                selectedAssetPath.clear();
                if (!contentCatalogRejected) contentBrowserStatus.clear();
            }
            if (contentTileView) drawTileBorder(tilePosition, width, height);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open folder\n%s", folder.path.c_str());
            ImGui::PopID();
        };
        if (contentTileView)
            drawTiles(browser.directories.size(), 82.0f, drawFolder);
        else
            drawList(browser.directories.size(), [&](size_t index) { drawFolder(index, 0, 0); });
        if (browser.directories.empty() && !contentCatalogRejected) ImGui::TextDisabled("No child folders");
    }
    ImGui::EndChild();

    ImGui::TextDisabled("%s", contentFilter[0] ? "Matching files" : "Files");
    if (ImGui::BeginChild("assets", ImVec2(0, -82), true)) {
        auto drawAsset = [&](size_t index, float width, float height) {
            const auto& item = browser.files[index];
            ImGui::PushID(item.path.c_str());
            const bool supported = item.supportedModelFormat();
            const std::string label = contentTileView
                ? (supported ? "[3D MODEL]\n" : "[UNSUPPORTED]\n") + item.name
                : item.name + "  [" + (supported ? asset::FormatName(item.format) : "unsupported") + "]";
            const ImVec2 size = contentTileView ? ImVec2(width, height) : ImVec2(0, 0);
            const ImVec2 tilePosition = ImGui::GetCursorScreenPos();
            if (contentTileView) drawTileBackdrop(tilePosition, width, height);
            if (ImGui::Selectable(label.c_str(), selectedAssetPath == item.path,
                                  ImGuiSelectableFlags_AllowDoubleClick, size)) {
                selectedAssetPath = item.path;
                contentBrowserStatus.clear();
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (supported && vfs.Exists(item.path))
                        Spawn(item.path);  // Preserve the existing double-click quick-place gesture.
                }
            }
            if (ImGui::IsItemHovered()) {
                if (!vfs.Exists(item.path)) {
                    ImGui::SetTooltip("%s\nStill present in the indexed listing, but unavailable from the mounted VFS now.\nRemount the source to refresh the listing.",
                                      item.path.c_str());
                } else if (supported) {
                    ImGui::SetTooltip("%s\n%llu bytes\nfrom %s\nRecognized %s format; the model loader checks its contents and dependencies on open.\nDouble-click to quick-place.",
                                      item.path.c_str(), static_cast<unsigned long long>(item.size),
                                      vfs.Resolve(item.path).c_str(), asset::FormatName(item.format));
                } else {
                    ImGui::SetTooltip("%s\n%llu bytes\nfrom %s\nVisible in the mounted VFS, but this file type is not supported for model placement.",
                                      item.path.c_str(), static_cast<unsigned long long>(item.size),
                                      vfs.Resolve(item.path).c_str());
                }
            }
            if (contentTileView) drawTileBorder(tilePosition, width, height);
            ImGui::PopID();
        };
        if (contentTileView)
            drawTiles(browser.files.size(), 82.0f, drawAsset);
        else
            drawList(browser.files.size(), [&](size_t index) { drawAsset(index, 0, 0); });
        if (browser.files.empty() && !contentCatalogRejected)
            ImGui::TextDisabled(contentFilter[0] ? "No files match this search in the current folder" :
                                                  "No files in the current folder");
    }
    ImGui::EndChild();

    const auto* selectedItem = asset::FindBrowserFile(browser, selectedAssetPath);
    const bool selectedExists = !selectedAssetPath.empty() && vfs.Exists(selectedAssetPath);
    const bool canPlace = selectedItem && selectedItem->supportedModelFormat() && selectedExists;
    if (!selectedAssetPath.empty()) {
        ImGui::BeginDisabled(!canPlace);
        if (ImGui::Button("Place with click")) StartPlacement(selectedAssetPath);
        ImGui::EndDisabled();
    }
    if (placement.active) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) CancelPlacement();
    }
    std::string selectionStatus;
    if (!selectedAssetPath.empty()) {
        if (!selectedExists)
            selectionStatus = "Selected asset is no longer available in the mounted VFS.";
        else if (!selectedItem)
            selectionStatus = "Selected asset is outside the current folder or search results.";
        else if (!selectedItem->supportedModelFormat())
            selectionStatus = "Selected file is visible but its format is not supported for model placement.";
    }
    if (browser.omittedDirectories || browser.omittedFiles)
        ImGui::TextWrapped("Showing at most 4,096 folders and 4,096 files; %llu folders and %llu files were omitted. Enter a folder or refine the search.",
                           static_cast<unsigned long long>(browser.omittedDirectories),
                           static_cast<unsigned long long>(browser.omittedFiles));
    if (!selectionStatus.empty()) ImGui::TextWrapped("%s", selectionStatus.c_str());
    else if (!contentBrowserStatus.empty()) ImGui::TextWrapped("%s", contentBrowserStatus.c_str());
    if (placement.active) ImGui::TextWrapped("%s", placement.status.c_str());
    ImGui::TextWrapped("Folders are inferred from mounted file paths. Mount a directory, .zip, or .scpak in Mounts to browse it through the VFS. Unsupported files stay visible; missing files are not listed. Directory mounts show their indexed snapshot, so remount after changing files on disk.");
    ImGui::End();
}

void Editor::DrawMounts() {
    if (!ImGui::Begin("Mounts", &showMounts)) { ImGui::End(); return; }
    if (ImGui::BeginTable("mounts", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("label");
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("mount point");
        ImGui::TableSetupColumn("priority");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        std::optional<vfs::MountId> drop;
        for (const auto& m : vfs.Mounts()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.label.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.kind.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.mountPoint.empty() ? "/" : m.mountPoint.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", m.priority);
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(m.id));
            if (ImGui::SmallButton("unmount")) drop = m.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (drop && vfs.Unmount(*drop)) {
            mountedSources.erase(*drop);
            InvalidateAssetBrowser();
        }
    }
    ImGui::SeparatorText("Mount");
    ImGui::InputTextWithHint("path", "directory, .zip or .scpak", mountPath, sizeof mountPath);
    ImGui::InputTextWithHint("mount point", "/ (root)", mountPoint, sizeof mountPoint);
    ImGui::InputInt("priority", &mountPriority);
    if (ImGui::Button("Mount")) {
        if (auto st = MountAssetSource(PathFromUtf8(mountPath), mountPoint, mountPriority); !st)
            log::Error("vfs", st.error);
        else InvalidateAssetBrowser();
    }
    ImGui::End();
}

void Editor::DrawScene() {
    if (!ImGui::Begin("Scene", &showScene)) { ImGui::End(); return; }
    if (projectStore) ImGui::TextWrapped("Project: %s  ·  scene: %s", projectRoot.generic_string().c_str(), document.id.c_str());
    else ImGui::TextDisabled("In-memory scene · use File > Open project to save in a project");
    ImGui::SeparatorText("Viewport gizmo");
    ImGui::BeginDisabled(transformGizmoGestureActive);
    if (ImGui::RadioButton("Move axes", gizmoMode == TransformGizmoMode::Move))
        gizmoMode = TransformGizmoMode::Move;
    ImGui::SameLine();
    if (ImGui::RadioButton("Scale axes", gizmoMode == TransformGizmoMode::Scale))
        gizmoMode = TransformGizmoMode::Scale;
    ImGui::SameLine();
    if (ImGui::RadioButton("Rotate rings", gizmoMode == TransformGizmoMode::Rotate))
        gizmoMode = TransformGizmoMode::Rotate;
    ImGui::EndDisabled();
    ImGui::SeparatorText("Transform snapping");
    const auto constrainStep = [](float& value, float fallback, float minimum, float maximum) {
        if (!std::isfinite(value)) value = fallback;
        value = std::clamp(value, minimum, maximum);
    };
    auto& snap = gizmoSnapSettings;
    constrainStep(snap.translationStep, 1.0f, render::kMinTranslationSnapStep,
                  render::kMaxTranslationSnapStep);
    constrainStep(snap.rotationStepDegrees, 15.0f, render::kMinRotationSnapStepDegrees,
                  render::kMaxRotationSnapStepDegrees);
    constrainStep(snap.scaleStep, 0.1f, render::kMinScaleSnapStep, render::kMaxScaleSnapStep);
    ImGui::BeginDisabled(transformGizmoGestureActive);
    ImGui::Checkbox("Move snap (world units)", &snap.translationEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::DragFloat("##move-snap-step", &snap.translationStep, 0.05f,
                     render::kMinTranslationSnapStep, render::kMaxTranslationSnapStep,
                     "%.3f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Rotate snap (degrees)", &snap.rotationEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::DragFloat("##rotate-snap-step", &snap.rotationStepDegrees, 0.5f,
                     render::kMinRotationSnapStepDegrees, render::kMaxRotationSnapStepDegrees,
                     "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Scale snap (local delta)", &snap.scaleEnabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::DragFloat("##scale-snap-step", &snap.scaleStep, 0.01f,
                     render::kMinScaleSnapStep, render::kMaxScaleSnapStep,
                     "%.3f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::TextDisabled("Snapping quantizes each drag from its starting transform; all modes are off by default.");
    if (gizmoMode == TransformGizmoMode::Scale)
        ImGui::TextDisabled("Scale follows local axes through the parent transform; "
                            "only positive scale is supported.");
    else if (gizmoMode == TransformGizmoMode::Rotate)
        ImGui::TextDisabled("Rotate around the node's local axis; nonuniform parents distort ring shape.");
    ImGui::BeginDisabled(!sceneHistory.CanUndo());
    if (ImGui::Button("Undo")) UndoScene();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!sceneHistory.CanRedo());
    if (ImGui::Button("Redo")) RedoScene();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (projectStore) {
        if (ImGui::Button("Save scene")) SaveScene();
        ImGui::SameLine();
        ImGui::TextUnformatted(documentDirty.dirty() ? "Unsaved" : "Saved");
    }
    if (ImGui::Button("Browse scenes")) OpenSceneBrowser();
    if (ImGui::Button("Add group")) AddGroup();
    const auto failedAssets = FailedSceneAssetHandles();
    if (!failedAssets.empty()) {
        ImGui::SameLine();
        const std::string retryLabel = "Retry failed assets (" + std::to_string(failedAssets.size()) + ")";
        if (ImGui::Button(retryLabel.c_str())) RetryFailedSceneAssets();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("After mounting or correcting assets, retry failed scene model loads. Ready models are reused.");
    }
    if (const scene::Node* selected = FindNode(selectedNode)) {
        const std::string selectedId = selected->id;
        const std::string selectedName = selected->name;
        const scene::NodeKind selectedKind = selected->kind;
        ImGui::SeparatorText("Selected node");
        if (renameTarget != selectedId) {
            renameTarget = selectedId;
            std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", selectedName.c_str());
            renameSourceName = selectedName;
            renameBufferDirty = false;
        } else if (!renameBufferDirty && renameSourceName != selectedName) {
            std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", selectedName.c_str());
            renameSourceName = selectedName;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##node-name", "Node name", renameBuffer, sizeof(renameBuffer)))
            renameBufferDirty = true;
        ImGui::BeginDisabled(!renameBufferDirty);
        if (ImGui::Button("Apply name")) {
            scene::Document candidate = document;
            if (auto status = scene::RenameNode(candidate, selectedId, renameBuffer); !status) {
                sceneStatus = "Rename refused: " + status.error;
            } else if (CommitDocument(std::move(candidate))) {
                if (const scene::Node* renamed = FindNode(selectedId)) {
                    renameSourceName = renamed->name;
                    renameBufferDirty = false;
                }
            }
        }
        ImGui::EndDisabled();

        const scene::Node* currentSelected = FindNode(selectedId);
        if (reparentTargetNode != selectedId) {
            reparentTargetNode = selectedId;
            reparentTarget = currentSelected ? currentSelected->parentId.value_or("") : "";
        }
        const scene::Node* target = reparentTarget.empty() ? nullptr : FindNode(reparentTarget);
        const std::string targetLabel = target && target->kind == scene::NodeKind::Group ? target->name : "Root";
        if (ImGui::BeginCombo("Parent group", targetLabel.c_str())) {
            if (ImGui::Selectable("Root", reparentTarget.empty())) reparentTarget.clear();
            for (const auto& group : document.nodes) {
                if (group.kind != scene::NodeKind::Group || IsDescendantOrSelf(group.id, selectedId)) continue;
                ImGui::PushID(group.id.c_str());
                if (ImGui::Selectable(group.name.c_str(), reparentTarget == group.id)) reparentTarget = group.id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        const std::string currentParent = currentSelected ? currentSelected->parentId.value_or("") : "";
        ImGui::BeginDisabled(reparentTarget == currentParent);
        if (ImGui::Button("Reparent (keep world)")) {
            scene::Document candidate = document;
            std::optional<std::string> newParent;
            if (!reparentTarget.empty()) newParent = reparentTarget;
            if (auto status = scene::ReparentNode(candidate, selectedId, std::move(newParent), true); !status) {
                sceneStatus = "Reparent refused: " + status.error;
            } else if (CommitDocument(std::move(candidate))) {
                sceneStatus = "Node reparented · unsaved";
            }
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(selectedNode.empty());
        if (ImGui::Button("Duplicate subtree (Ctrl+D)")) DuplicateSelected();
        const char* deleteLabel = selectedKind == scene::NodeKind::Group ? "Delete group + children" : "Delete node";
        if (ImGui::Button(deleteLabel)) RemoveSelected();
        ImGui::EndDisabled();
    }
    if (!sceneStatus.empty()) ImGui::TextWrapped("%s", sceneStatus.c_str());
    ImGui::Separator();

    std::unordered_map<std::string, std::vector<const scene::Node*>> children;
    std::vector<const scene::Node*> roots;
    for (const auto& node : document.nodes) {
        if (node.parentId) children[*node.parentId].push_back(&node);
        else roots.push_back(&node);
    }
    const auto drawNode = [&](const auto& self, const scene::Node& node) -> void {
        const auto foundChildren = children.find(node.id);
        const bool hasChildren = foundChildren != children.end() && !foundChildren->second.empty();
        ImGui::PushID(node.id.c_str());
        if (node.kind == scene::NodeKind::Group) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
            if (selectedNode == node.id) flags |= ImGuiTreeNodeFlags_Selected;
            if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            const bool open = ImGui::TreeNodeEx("node", flags, "%s", node.name.c_str());
            if (ImGui::IsItemClicked()) selectedNode = node.id;
            if (open && hasChildren) {
                for (const auto* child : foundChildren->second) self(self, *child);
                ImGui::TreePop();
            }
        } else {
            const Instance* inst = nullptr;
            for (const auto& candidate : instances) if (candidate.nodeId == node.id) { inst = &candidate; break; }
            std::string label = node.name;
            bool failed = false;
            std::string loadError;
            if (inst && !inst->model) {
                const auto info = loader->Info(inst->handle);
                failed = info.state == asset::LoadState::Failed;
                label += failed ? "  (failed)" : "  (loading)";
                loadError = info.error;
            } else if (!inst) label += "  (unavailable)";
            if (ImGui::Selectable(label.c_str(), selectedNode == node.id, ImGuiSelectableFlags_SpanAvailWidth))
                selectedNode = node.id;
            if (failed && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", loadError.c_str());
        }
        ImGui::PopID();
    };
    for (const auto* root : roots) drawNode(drawNode, *root);
    if (document.nodes.empty()) ImGui::TextDisabled("Scene is empty. Choose a model in Content to place it.");
    if (placement.active) ImGui::TextWrapped("Placement preview: %s · click viewport to place · Esc cancels",
                                               placement.assetPath.c_str());
    ImGui::End();
}

void Editor::DrawSceneBrowser() {
    if (!showSceneBrowser) return;
    if (!ImGui::Begin("Project scenes", &showSceneBrowser)) { ImGui::End(); return; }
    if (ImGui::Button("Refresh")) RefreshProjectScenes();
    ImGui::SameLine();
    ImGui::TextWrapped("Project: %s", projectRoot.generic_string().c_str());
    ImGui::BeginChild("project-scenes", {0, 180}, ImGuiChildFlags_Borders);
    if (projectScenes.empty()) ImGui::TextDisabled("No saved scenes in this project.");
    for (const auto& id : projectScenes) {
        const std::string label = id == document.id ? id + "  (current)" : id;
        if (ImGui::Selectable(label.c_str(), selectedProjectScene == id)) selectedProjectScene = id;
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(selectedProjectScene.empty() || selectedProjectScene == document.id);
    if (ImGui::Button("Open selected")) RequestOpenScene(selectedProjectScene);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Create scene");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##new-scene-id", "scene ID (for example, hangar)", newSceneId, sizeof(newSceneId));
    ImGui::BeginDisabled(newSceneId[0] == '\0');
    if (ImGui::Button("Create scene")) RequestCreateScene(newSceneId);
    ImGui::EndDisabled();
    if (!sceneBrowserStatus.empty()) ImGui::TextWrapped("%s", sceneBrowserStatus.c_str());
    ImGui::End();
}

void Editor::DrawProjectOpenDialog() {
    if (!showProjectOpenDialog) return;
    const char* title = createProjectDialog ? "Create project" : "Open project";
    if (!ImGui::Begin(title, &showProjectOpenDialog)) { ImGui::End(); return; }
    if (createProjectDialog) {
        ImGui::TextWrapped("Choose a new folder inside an existing parent directory. The editor creates an empty main scene; existing folders and files are never replaced. Mounted asset roots are read-only and cannot contain the project.");
    } else {
        ImGui::TextWrapped("Enter the path to an existing project folder. The editor opens its main scene, or starts a new unsaved main scene if none exists.");
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##project-path", createProjectDialog
                                 ? "C:\\Users\\you\\Documents\\NewProject"
                                 : "C:\\Users\\you\\Documents\\MyProject",
                             projectPathBuffer, sizeof(projectPathBuffer));
    ImGui::BeginDisabled(projectPathBuffer[0] == '\0');
    if (createProjectDialog) {
        if (ImGui::Button("Create project")) RequestCreateProject();
    } else if (ImGui::Button("Open project")) {
        RequestOpenProject();
    }
    ImGui::EndDisabled();
    if (!projectOpenStatus.empty()) ImGui::TextWrapped("%s", projectOpenStatus.c_str());
    if (!projectRoot.empty()) ImGui::TextDisabled("Current project: %s", projectRoot.generic_string().c_str());
    ImGui::End();
}

void Editor::DrawSceneChangeConfirmation(bool& quit) {
    ImGui::SetNextWindowPos({-1000, -1000}, ImGuiCond_Always);
    ImGui::SetNextWindowSize({1, 1}, ImGuiCond_Always);
    const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##SceneActionPopupHost", nullptr, hostFlags);
    const auto& pending = sceneActionGate.pending();
    if (!pending) {
        ImGui::End();
        return;
    }
    if (pendingSceneConfirmation) {
        ImGui::OpenPopup("Confirm scene change");
        pendingSceneConfirmation = false;
    }
    if (!ImGui::BeginPopupModal("Confirm scene change", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    const bool quitting = pending->kind == scene::SceneActionKind::Quit;
    const bool projectChange = pending->kind == scene::SceneActionKind::OpenProject ||
                               pending->kind == scene::SceneActionKind::CreateProject;
    const bool creatingProject = pending->kind == scene::SceneActionKind::CreateProject;
    if (quitting)
        ImGui::TextWrapped("Scene %s has unsaved changes. Save them before quitting?", document.id.c_str());
    else if (projectChange && creatingProject)
        ImGui::TextWrapped("Scene %s has unsaved changes. Save them before creating a new project?", document.id.c_str());
    else if (projectChange)
        ImGui::TextWrapped("Scene %s has unsaved changes. Save them before opening another project?", document.id.c_str());
    else
        ImGui::TextWrapped("Scene %s has unsaved changes. Save them before switching?", document.id.c_str());
    ImGui::Separator();
    const char* saveLabel = quitting ? "Save and quit" : "Save and continue";
    ImGui::BeginDisabled(!projectStore.has_value());
    if (ImGui::Button(saveLabel)) {
        SaveScene();
        auto action = sceneActionGate.Resolve(scene::SceneActionChoice::Save, !documentDirty.dirty());
        if (action) {
            CompletePendingSceneAction(*action, &quit);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const char* discardLabel = quitting ? "Discard changes and quit" : "Discard changes and continue";
    if (ImGui::Button(discardLabel)) {
        auto action = sceneActionGate.Resolve(scene::SceneActionChoice::Discard);
        if (action) CompletePendingSceneAction(*action, &quit);
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("Cancel")) {
        sceneActionGate.Resolve(scene::SceneActionChoice::Cancel);
        pendingSceneConfirmation = false;
        ImGui::CloseCurrentPopup();
    }
    if (!projectStore && (quitting || projectChange))
        ImGui::TextWrapped("This in-memory scene has no project file; Save is unavailable. Choose Discard or Cancel.");
    else if (!documentDirty.dirty() && sceneActionGate.pending())
        ImGui::TextWrapped("Save completed; the scene switch is waiting to finish.");
    else if (sceneStatus.starts_with("Save failed") || sceneStatus.starts_with("Saved, but"))
        ImGui::TextWrapped("%s", sceneStatus.c_str());
    ImGui::EndPopup();
    ImGui::End();
}

void Editor::DrawInspector() {
    if (!ImGui::Begin("Inspector", &showInspector)) {
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        inspectorRotationInitialized = false;
        inspectorRotationActive = false;
        ImGui::End();
        return;
    }
    scene::Node* selected = FindNode(selectedNode);
    if (!selected) {
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        inspectorRotationNode.clear();
        inspectorRotationInitialized = false;
        inspectorRotationActive = false;
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }
    if (inspectorRotationNode != selected->id) {
        inspectorRotationNode = selected->id;
        inspectorRotationInitialized = false;
        inspectorRotationActive = false;
    }
    if (!inspectorRotationInitialized || !inspectorRotationActive) {
        if (auto status = scene::QuaternionToEulerXYZDegrees(selected->localTransform.rotation,
                                                             inspectorRotationDegrees); !status) {
            sceneStatus = "Cannot display rotation: " + status.error;
            inspectorRotationDegrees = {};
        }
        inspectorRotationInitialized = true;
    }
    ImGui::Text("%s", selected->name.c_str());
    Instance* inst = selected->kind == scene::NodeKind::ModelInstance ? FindNodeInstance(selected->id) : nullptr;
    if (inst && inst->model)
        ImGui::TextDisabled("%s  %zu mesh(es)  %zu triangles", inst->model->path.c_str(),
                            inst->model->meshes.size(), inst->model->triangleCount());
    ImGui::SeparatorText("Local transform");
    scene::Document candidate = document;
    auto nodeIt = std::find_if(candidate.nodes.begin(), candidate.nodes.end(),
                               [&](const scene::Node& node) { return node.id == selectedNode; });
    if (nodeIt == candidate.nodes.end()) {
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
        ImGui::End();
        return;
    }
    bool changed = ImGui::DragFloat3("translation", &nodeIt->localTransform.translation.x, 0.05f);
    bool anyActive = ImGui::IsItemActive();
    const bool rotationChanged = ImGui::DragFloat3("local rotation XYZ (degrees)",
        &inspectorRotationDegrees.x, 0.5f, -scene::kMaxRotationDegrees,
        scene::kMaxRotationDegrees, "%.1f");
    const bool rotationActive = ImGui::IsItemActive();
    anyActive |= rotationActive;
    if (rotationChanged) {
        scene::Quaternion rotation;
        if (auto status = scene::EulerXYZDegreesToQuaternion(inspectorRotationDegrees, rotation); status) {
            nodeIt->localTransform.rotation = rotation;
            changed = true;
        } else {
            if (auto restore = scene::QuaternionToEulerXYZDegrees(selected->localTransform.rotation,
                                                                  inspectorRotationDegrees); !restore)
                inspectorRotationDegrees = {};
            sceneStatus = "Rotation edit refused: " + status.error;
        }
    }
    inspectorRotationActive = rotationActive;
    const bool scaleChanged = ImGui::DragFloat3("scale", &nodeIt->localTransform.scale.x, 0.01f, 0.001f, 1000.0f);
    changed |= scaleChanged;
    anyActive |= ImGui::IsItemActive();
    std::optional<float> uniformScaleFactor;
    if (ImGui::Button("Scale uniformly ÷1.1")) uniformScaleFactor = 1.0f / 1.1f;
    ImGui::SameLine();
    if (ImGui::Button("Scale uniformly ×1.1")) uniformScaleFactor = 1.1f;
    if (uniformScaleFactor) {
        if (auto status = scene::ScaleUniform(nodeIt->localTransform, *uniformScaleFactor); !status)
            sceneStatus = "Scale refused: " + status.error;
        else
            changed = true;
    }
    if (changed) {
        if (inspectorUndoNode != selectedNode) {
            inspectorUndoCaptured = false;
            inspectorUndoNode = selectedNode;
        }
        CommitInteractiveDocument(std::move(candidate), inspectorUndoCaptured);
        inst = FindNodeInstance(selectedNode);
    }
    if (!anyActive) {
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
    }
    if (inst) {
        ImGui::SeparatorText("Material (session only)");
        ImGui::ColorEdit3("base color", &inst->material.baseColor.x);
        ImGui::SliderFloat("metallic", &inst->material.metallic, 0, 1);
        ImGui::SliderFloat("roughness", &inst->material.roughness, 0.04f, 1);
    }
    if (inst) {
        if (ImGui::Button("Frame (F)")) FrameInstance(*inst);
        ImGui::SameLine();
    }
    if (ImGui::Button("Delete (Del)")) RemoveSelected();
    ImGui::End();
}

void Editor::DrawPlugins() {
    if (ImGui::Begin("Plugins", &showPlugins)) {
        if (!sco.started()) ImGui::TextDisabled("sco-core host not started");
        if (ImGui::BeginTable("plugins", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("plugin");
            ImGui::TableSetupColumn("version");
            ImGui::TableSetupColumn("state");
            ImGui::TableSetupColumn("reason");
            ImGui::TableHeadersRow();
            for (const auto& p : sco.Plugins()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.id.empty() ? p.folder.c_str() : p.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.version.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.state.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.reason.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    // Every sco.widgets panel a plugin publishes gets a dockable window. The SDK's adapter draws
    // it; events go back through sco-core's validation to the owning plugin.
    static sco::widgets::ImGuiRenderer widgets;
    for (const auto& panel : sco.Panels()) {
        const std::string title = (panel.title.empty() ? panel.id : panel.title) + "  [" + panel.owner + "]###panel:" + panel.id;
        ImGui::SetNextWindowSize({360, 420}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title.c_str())) {
            if (auto st = sco.RenderPanel(panel.id, widgets); !st) ImGui::TextDisabled("%s", st.error.c_str());
        }
        ImGui::End();
    }
}

void Editor::DrawLoader() {
    if (!ImGui::Begin("Asset loader", &showLoader)) { ImGui::End(); return; }
    ImGui::Text("workers %u  queued %zu  in flight %zu", jobs.workers(), jobs.queued(), loader->inFlight());
    if (ImGui::BeginTable("loads", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("path");
        ImGui::TableSetupColumn("state");
        ImGui::TableSetupColumn("bytes");
        ImGui::TableSetupColumn("read ms");
        ImGui::TableSetupColumn("decode ms");
        ImGui::TableHeadersRow();
        for (asset::ModelHandle h : requested) {
            const auto info = loader->Info(h);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(info.path.c_str());
            if (!info.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.error.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(asset::LoadStateName(info.state));
            ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(info.bytes));
            ImGui::TableNextColumn(); ImGui::Text("%.2f", info.readMs);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", info.decodeMs);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::DrawStats() {
    if (!ImGui::Begin("Stats", &showStats)) { ImGui::End(); return; }
    const auto s = renderer->Stats();
    ImGui::Text("%s on %s", s.backend.c_str(), s.adapter.c_str());
    ImGui::Text("%.1f fps  record %.2f ms", ImGui::GetIO().Framerate, s.cpuRecordMs);
    ImGui::Text("draws %u  instances %u  resident models %u  pending uploads %u", s.drawCalls, s.instances, s.residentModels, s.pendingUploads);
    ImGui::End();
}

void Editor::DrawLog() {
    if (!ImGui::Begin("Log", &showLog)) { ImGui::End(); return; }
    for (auto& l : log::Since(logSeen)) {
        logSeen = l.seq;
        logLines.push_back(std::move(l));
    }
    if (logLines.size() > 4000) logLines.erase(logLines.begin(), logLines.begin() + 1000);
    if (ImGui::SmallButton("Clear")) logLines.clear();
    ImGui::BeginChild("lines");
    for (const auto& l : logLines) {
        const ImVec4 color = l.level == log::Level::Error ? ImVec4{1.0f, 0.45f, 0.4f, 1} : l.level == log::Level::Warn ? ImVec4{1.0f, 0.8f, 0.4f, 1} : ImVec4{0.75f, 0.78f, 0.82f, 1};
        ImGui::TextColored(color, "%s: %s", l.channel.c_str(), l.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void Editor::DrawScenario() {
    if (!ImGui::Begin("Scenario", &showScenario)) { ImGui::End(); return; }
    ImGui::TextDisabled("Bounded synthetic controls; nothing here has run in Star Citizen.");
    ImGui::SetNextItemWidth(-90);
    ImGui::InputTextWithHint("##scenario", "scenario .json", scenarioPath, sizeof scenarioPath);
    ImGui::SameLine();
    if (ImGui::Button("Run") && scenarioPath[0] != '\0') RunScenarioFile(scenarioPath);
    if (!scenarioStatus.empty()) {
        const ImVec4 color = scenarioPassed ? ImVec4{0.5f, 0.85f, 0.5f, 1} : ImVec4{1.0f, 0.45f, 0.4f, 1};
        ImGui::TextColored(color, "%s", scenarioStatus.c_str());
    }
    if (!scenarioResults.empty()) {
        ImGui::BeginChild("steps");
        if (ImGui::BeginTable("steps", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28);
            ImGui::TableSetupColumn("control");
            ImGui::TableSetupColumn("outcome", ImGuiTableColumnFlags_WidthFixed, 64);
            ImGui::TableSetupColumn("detail");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < scenarioResults.size(); ++i) {
                const auto& r = scenarioResults[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i + 1);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.control.c_str());
                ImGui::TableNextColumn();
                const ImVec4 color = r.matched ? ImVec4{0.5f, 0.85f, 0.5f, 1} : ImVec4{1.0f, 0.45f, 0.4f, 1};
                ImGui::TextColored(color, "%s", r.observed ? "ok" : "refused");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.detail.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

void Editor::DrawProduct() {
    if (!ImGui::Begin("Product", &showProduct)) { ImGui::End(); return; }
    if (productRoot.empty()) {
        ImGui::TextDisabled("No product profile mounted. Start the editor with --product DIR.");
        ImGui::End();
        return;
    }
    ImGui::Text("root  %s", productRoot.generic_string().c_str());
    ImGui::TextDisabled("Writes a distribution that --product accepts again; nothing leaves this machine.");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##export", "output directory or .scpak", exportPath, sizeof exportPath);
    if (ImGui::RadioButton("directory", !exportPack)) exportPack = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(".scpak", exportPack)) exportPack = true;
    ImGui::SameLine();
    if (ImGui::Button("Export") && exportPath[0] != '\0') {
        product::ExportReport report;
        const fs::path out = PathFromUtf8(exportPath);
        const Status st = exportPack ? product::ExportProductPack(productRoot, out, report)
                                     : product::ExportProduct(productRoot, out, report);
        if (!st) {
            exportStatus = "failed: " + st.error;
            log::Error("product", st.error);
        } else {
            exportStatus = "exported " + std::to_string(report.files) + " file(s), " +
                           std::to_string(report.bytes) + " byte(s), " + std::to_string(report.plugins.size()) +
                           " plugin(s) to " + out.generic_string();
            log::Info("product", exportStatus);
        }
    }
    if (!exportStatus.empty()) ImGui::TextWrapped("%s", exportStatus.c_str());
    ImGui::SeparatorText("Roster");
    for (const auto& p : sco.Plugins())
        ImGui::Text("%s  %s%s%s", p.id.empty() ? p.folder.c_str() : p.id.c_str(), p.state.c_str(),
                    p.reason.empty() ? "" : " - ", p.reason.c_str());
    ImGui::End();
}

void Editor::DrawSurface() {
    if (!ImGui::Begin("Plugin surface", &showSurface)) { ImGui::End(); return; }
    const auto panels = sco.Panels();
    if (panels.empty()) {
        ImGui::TextWrapped("No plugin panels published. Load a plugin with a sco.widgets panel.");
        surfaceEnabled = false;
    } else {
        ImGui::Checkbox("World-space plugin surface", &surfaceEnabled);
        if (ImGui::BeginCombo("Panel", surfacePanel.c_str())) {
            for (const auto& p : panels) {
                const std::string label = (p.title.empty() ? p.id : p.title) + " [" + p.owner + "]###" + p.id;
                if (ImGui::Selectable(label.c_str(), surfacePanel == p.id)) {
                    surfacePanel = p.id;
                    surfaceFocused = false;
                    surfaceDragging = false;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::TextWrapped("512 x 384 independent plugin UI. Click the quad to focus; click outside or Escape to release. Hiding this dock does not hide the quad.");
    ImGui::TextUnformatted(surfaceFocused ? "Surface focused" : "Editor focused");
    if (!surfaceStatus.empty()) ImGui::TextWrapped("%s", surfaceStatus.c_str());
    ImGui::End();
}

void Editor::DrawUi(bool& quit) {
    if (showContent) DrawContent();
    if (showMounts) DrawMounts();
    if (showScene) DrawScene();
    if (showInspector) DrawInspector();
    else {
        inspectorUndoCaptured = false;
        inspectorUndoNode.clear();
    }
    DrawSceneBrowser();
    DrawProjectOpenDialog();
    DrawSceneChangeConfirmation(quit);
    if (showPlugins) DrawPlugins();
    if (showLoader) DrawLoader();
    if (showStats) DrawStats();
    if (showLog) DrawLog();
    if (showScenario) DrawScenario();
    if (showProduct) DrawProduct();
    if (showSurface) DrawSurface();
    // No exporter callback: the dock explicitly disables folder export; source copy works.
    // Do not claim a folder receipt without hostd write + readback verification.
    designer.Draw();
    if (showNetSandbox) {
        if (netSandbox) netSandbox->Draw(&showNetSandbox);
        else {
            if (ImGui::Begin("NetSandbox", &showNetSandbox))
                ImGui::TextWrapped("Unavailable: this build has no absolute SCUBA_DEFAULT_HOSTD executable path.");
            ImGui::End();
        }
    }
    if (showDemo) ImGui::ShowDemoWindow(&showDemo);
    DrawTransformGizmo();
}

void Editor::DrawTransformGizmo() const {
    if (placement.active) return;  // PlacementTool has pointer precedence over every gizmo mode.
    TransformGizmoGeometry geometry;
    if (!BuildGizmoGeometry(geometry)) return;
    const auto viewport = ui.viewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect({viewport.x, viewport.y}, {viewport.x + viewport.w, viewport.y + viewport.h}, true);
    constexpr ImU32 colors[3] = {IM_COL32(236, 82, 82, 255), IM_COL32(86, 214, 126, 255),
                                 IM_COL32(84, 154, 250, 255)};
    constexpr const char* labels[3] = {"X", "Y", "Z"};
    if (gizmoMode == TransformGizmoMode::Rotate) {
        for (size_t axis = 0; axis < 3; ++axis) {
            const ImU32 color = transformGizmoDragging && transformGizmoAxis == static_cast<int>(axis)
                ? IM_COL32(255, 236, 128, 255) : colors[axis];
            const size_t first = axis * render::kRotationRingSegments;
            for (size_t segmentIndex = 0; segmentIndex < render::kRotationRingSegments; ++segmentIndex) {
                const auto& segment = geometry.rotationHandles[first + segmentIndex];
                const float dx = segment.end.x - segment.start.x;
                const float dy = segment.end.y - segment.start.y;
                if (!std::isfinite(dx) || !std::isfinite(dy) || dx * dx + dy * dy < 1.0f) continue;
                draw->AddLine({segment.start.x, segment.start.y}, {segment.end.x, segment.end.y}, color, 2.5f);
            }
            const auto& labelSegment = geometry.rotationHandles[first];
            if (std::hypot(labelSegment.end.x - geometry.center.x,
                           labelSegment.end.y - geometry.center.y) >= 12.0f)
                draw->AddText({labelSegment.end.x + 4.0f, labelSegment.end.y - 7.0f}, color, labels[axis]);
        }
        draw->AddCircleFilled({geometry.center.x, geometry.center.y}, 4.0f, IM_COL32(28, 31, 38, 255));
        draw->PopClipRect();
        return;
    }
    for (size_t axis = 0; axis < geometry.axes.size(); ++axis) {
        const auto& segment = geometry.axes[axis];
        const float dx = segment.end.x - segment.start.x;
        const float dy = segment.end.y - segment.start.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(length) || length < 12.0f) continue;
        const float ux = dx / length, uy = dy / length;
        const float px = -uy, py = ux;
        const float headLength = std::min(11.0f, length * 0.25f);
        const float headWidth = std::min(5.0f, length * 0.12f);
        const ImVec2 tip{segment.end.x, segment.end.y};
        const ImVec2 headBase{segment.end.x - ux * headLength, segment.end.y - uy * headLength};
        const ImU32 color = transformGizmoDragging && transformGizmoAxis == static_cast<int>(axis)
            ? IM_COL32(255, 236, 128, 255) : colors[axis];
        draw->AddLine({segment.start.x, segment.start.y}, tip, color, 3.0f);
        if (gizmoMode == TransformGizmoMode::Scale) {
            draw->AddRectFilled({tip.x - 5.0f, tip.y - 5.0f}, {tip.x + 5.0f, tip.y + 5.0f}, color);
            draw->AddRect({tip.x - 5.0f, tip.y - 5.0f}, {tip.x + 5.0f, tip.y + 5.0f},
                          IM_COL32(25, 28, 34, 255), 0.0f, 1.5f, 0);
        } else {
            draw->AddLine(tip, {headBase.x + px * headWidth, headBase.y + py * headWidth}, color, 3.0f);
            draw->AddLine(tip, {headBase.x - px * headWidth, headBase.y - py * headWidth}, color, 3.0f);
        }
        draw->AddText({segment.end.x + 5.0f, segment.end.y - 7.0f}, color, labels[axis]);
    }
    draw->AddCircleFilled({geometry.center.x, geometry.center.y}, 5.0f, IM_COL32(28, 31, 38, 255));
    draw->AddCircle({geometry.center.x, geometry.center.y}, 5.0f, IM_COL32(235, 235, 235, 255), 0, 2.0f);
    draw->PopClipRect();
}

int Run(const Args& args) {
    log::SetSink(&LogSink);
    fs::path content = args.content;
    fs::path data = args.data;
    std::error_code ec;
    if (!args.product.empty()) {
        // A product profile is the whole distribution root: data/plugins must exist before any
        // window or GPU object is created, so a bad root fails fast instead of half-starting.
        const fs::path dataDir = args.product / "data";
        const fs::path pluginsDir = dataDir / "plugins";
        if (!fs::is_directory(pluginsDir, ec) || ec) {
            log::Error("app", "product profile requires " + pluginsDir.generic_string());
            return 1;
        }
        data = dataDir;
        content = fs::is_directory(dataDir, ec) && !ec ? dataDir : fs::path();
    }
    if (!args.exportDir.empty() || !args.exportPack.empty()) {
        // An export creates no window and no GPU object: it reads the mounted root and writes the
        // distribution, so it also works where the editor cannot open a window at all.
        product::ExportReport report;
        const fs::path& out = args.exportDir.empty() ? args.exportPack : args.exportDir;
        const Status st = args.exportDir.empty() ? product::ExportProductPack(args.product, out, report)
                                                 : product::ExportProduct(args.product, out, report);
        if (!st) { log::Error("product", st.error); return 1; }
        std::fprintf(stderr, "scuba-engine: exported %s: %zu file(s), %llu byte(s), %zu plugin(s)\n",
                     out.generic_string().c_str(), report.files,
                     static_cast<unsigned long long>(report.bytes), report.plugins.size());
        for (const auto& plugin : report.plugins) std::fprintf(stderr, "  %s\n", plugin.c_str());
        return 0;
    }

    platform::Window window;
    platform::WindowDesc wd;
    wd.title = "scuba-engine " SCUBA_VERSION;
    wd.visible = !args.hidden;
    if (auto st = window.Create(wd); !st) { log::Error("app", st.error); return 1; }

    Editor ed;
    ed.productRoot = args.product;
    render::RendererDesc rd;
    rd.window = window.native();
    rd.width = window.width();
    rd.height = window.height();
    rd.vsync = args.vsync;
    rd.debugLayer = args.debugLayer;
    rd.warp = args.warp;
    // --frames is the smoke path and has to run without a desktop: a hidden window cannot host a
    // swap chain, so the frames go to an off-screen target and nothing is presented.
    rd.offscreen = args.frames != 0 && args.hidden;
    Status st;
    ed.renderer = render::Renderer::CreateD3D12(rd, st);
    if (!ed.renderer) { log::Error("render", st.error); return 1; }
    if (st = ed.ui.Init(window, *ed.renderer); !st) { log::Error("ui", st.error); return 1; }
    ed.surface = std::make_unique<ui::PluginSurface>();
    if (st = ed.surface->Init(ImGui::GetCurrentContext()); !st) { log::Error("ui", st.error); return 1; }
#ifdef SCUBA_DEFAULT_HOSTD
    const fs::path hostdExecutable = PathFromUtf8(SCUBA_DEFAULT_HOSTD);
    if (hostdExecutable.is_absolute() && fs::is_regular_file(hostdExecutable, ec) && !ec)
        ed.netSandbox = std::make_unique<ui::NetSandbox>(hostdExecutable);
    else log::Warn("netsandbox", "built hostd executable unavailable: " + hostdExecutable.generic_string());
#endif

    if (!content.empty() && fs::is_directory(content))
        if (auto m = ed.MountAssetSource(content, "", 0); !m) log::Warn("vfs", m.error);
    for (const auto& m : args.mounts)
        if (auto r = ed.MountAssetSource(PathFromUtf8(m.path), m.mountPoint, m.priority); !r) log::Error("vfs", r.error);
    ed.loader = std::make_unique<asset::ModelLoader>(ed.vfs, ed.jobs);

    if (args.project.empty()) {
        if (auto sceneStatus = ed.InitializeEmptyScene(); !sceneStatus) {
            log::Error("scene", sceneStatus.error);
            return 1;
        }
    } else {
        if (auto sceneStatus = ed.OpenProject(args.project, args.sceneId); !sceneStatus) {
            log::Error("scene", sceneStatus.error);
            return 1;
        }
    }

    ScoHostOptions so;
    if (args.plugins_on && args.product.empty()) so.pluginRoot = args.plugins;
    so.productRoot = args.product;
    so.dataRoot = data;
    if (auto r = ed.sco.Start(so); !r) {
#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
        if (ed.sco.retained()) std::_Exit(EXIT_FAILURE);
#endif
        // A product profile is fail-closed: a refused plugin or an unusable root stops the editor.
        if (!args.product.empty()) { log::Error("sco", r.error); return 1; }
        log::Warn("sco", r.error);   // the editor still runs without plugins
    }
    if (!args.product.empty()) {
        const auto roster = ed.sco.Plugins();
        std::fprintf(stderr, "scuba-engine: product profile %s: %zu plugin(s)\n",
                     args.product.generic_string().c_str(), roster.size());
        for (const auto& p : roster)
            std::fprintf(stderr, "  %s %s%s%s\n", p.id.empty() ? p.folder.c_str() : p.id.c_str(), p.state.c_str(),
                         p.reason.empty() ? "" : " - ", p.reason.c_str());
    }

    const auto publishedPanels = ed.sco.Panels();
    ed.surfacePanel = args.panelId;
    if (ed.surfacePanel.empty() && !publishedPanels.empty()) ed.surfacePanel = publishedPanels.front().id;
    const bool selectedPanelExists = std::any_of(publishedPanels.begin(), publishedPanels.end(),
        [&](const auto& p) { return p.id == ed.surfacePanel; });
    if (args.diegetic && !selectedPanelExists) {
        log::Error("ui", "--diegetic requires a published plugin panel: " + ed.surfacePanel);
        return 1;
    }
    ed.surfaceEnabled = args.diegetic;
    // Allocate before submissions, including when enabled later from the picker dock.
    const render::GpuPanel panel = ed.renderer->CreatePanel(512, 384);
    if (!panel) { log::Error("render", "plugin surface creation failed"); return 1; }

    int code = 0;
    if (!args.scenario.empty()) {
        // Runs on the game thread before the first frame: a failing step has to be a nonzero exit,
        // not a line in a dock nobody is looking at.
        hostd::Json scenario;
        std::string error;
        if (!hostd::LoadScenario(args.scenario.generic_string(), scenario, error)) {
            log::Error("scenario", args.scenario.generic_string() + ": " + error);
            code = 1;
        } else if (!hostd::RunScenario(scenario)) {
            code = 1;
        }
    }

    for (const auto& p : args.open) ed.Spawn(p);

    UiBlocker blocker(ed);
    PlacementTool placementTool(ed);
    AxisTransformTool axisTransformTool(ed);
    MoveTool move(ed);
    CameraTool cameraTool(ed);
    ui::InputRouter router;
    router.Add(&blocker);          // UI owns clicks over panels and outside the viewport.
    router.Add(&placementTool);     // An active asset preview owns viewport clicks before gizmos.
    router.Add(&axisTransformTool); // Gizmo handles win before ordinary model picking.
    router.Add(&move);
    router.Add(&cameraTool);

    bool quit = false;
    int frame = 0;
    while (!quit && window.Pump()) {
        if (window.TakeResized()) ed.renderer->Resize(window.width(), window.height());
        ed.sco.Tick();
        ed.PumpLoader();

        ed.ui.BeginFrame();
        if (window.TakeCloseRequest()) ed.RequestQuit(quit);
        ImGuiIO& io = ImGui::GetIO();
        ed.UpdatePlacementPreview(io.MousePos.x, io.MousePos.y);
        ui::SurfaceInput surfaceInput;
        ui::PanelHit hit;
        const bool overSurface = ed.surfaceEnabled && !ed.surfacePanel.empty() &&
            !ed.ui.pointerOverUi() && ui::HitPanel(ed.camera, io.MousePos.x, io.MousePos.y,
                                                  ed.ui.viewport(), ed.SurfaceWorld(), hit);
        bool clicked = false, held = false;
        for (int b = 0; b < 5; ++b) {
            clicked |= io.MouseClicked[b];
            held |= io.MouseDown[b];
        }
        const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        const bool escape = (ed.surfaceFocused || ed.placement.active) && escapePressed;
        if (ed.placement.active && escapePressed) ed.CancelPlacement();
        if (!ed.surfaceEnabled || escape || (clicked && !overSurface)) {
            ed.surfaceFocused = false;
            ed.surfaceDragging = false;
        } else if (clicked && overSurface && !router.captured()) {
            ed.surfaceFocused = true;
            ed.surfaceDragging = true;
            ImGui::SetWindowFocus(nullptr);
        }
        surfaceInput.focused = ed.surfaceFocused;
        surfaceInput.pointer = overSurface;
        if (overSurface) {
            surfaceInput.x = hit.u * 512.0f;
            surfaceInput.y = hit.v * 384.0f;
        }
        for (int b = 0; b < 5; ++b)
            surfaceInput.buttons[b] = ed.surfaceFocused && io.MouseDown[b];
        if (ed.surfaceFocused) {
            if (overSurface || ed.surfaceDragging) {
                surfaceInput.wheelX = io.MouseWheelH;
                surfaceInput.wheelY = io.MouseWheel;
            }
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
                // Mouse aliases are generated by AddMouse*; submitting them as keys asserts.
                if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                surfaceInput.keys.push_back({key, ImGui::IsKeyDown(static_cast<ImGuiKey>(key))});
            }
            surfaceInput.keys.push_back({ImGuiMod_Ctrl, io.KeyCtrl});
            surfaceInput.keys.push_back({ImGuiMod_Shift, io.KeyShift});
            surfaceInput.keys.push_back({ImGuiMod_Alt, io.KeyAlt});
            surfaceInput.keys.push_back({ImGuiMod_Super, io.KeySuper});
            for (ImWchar character : io.InputQueueCharacters) {
                const unsigned c = static_cast<unsigned>(character);
                if (c < 0x80) surfaceInput.text += static_cast<char>(c);
                else if (c < 0x800) {
                    surfaceInput.text += static_cast<char>(0xc0u | (c >> 6u));
                    surfaceInput.text += static_cast<char>(0x80u | (c & 63u));
                } else if (c < 0x10000) {
                    surfaceInput.text += static_cast<char>(0xe0u | (c >> 12u));
                    surfaceInput.text += static_cast<char>(0x80u | ((c >> 6u) & 63u));
                    surfaceInput.text += static_cast<char>(0x80u | (c & 63u));
                } else {
                    surfaceInput.text += static_cast<char>(0xf0u | (c >> 18u));
                    surfaceInput.text += static_cast<char>(0x80u | ((c >> 12u) & 63u));
                    surfaceInput.text += static_cast<char>(0x80u | ((c >> 6u) & 63u));
                    surfaceInput.text += static_cast<char>(0x80u | (c & 63u));
                }
            }
        }
        for (const auto& ev : ed.ui.Events()) {
            if (ev.kind == ui::InputKind::KeyDown) {
                if (ed.surfaceFocused || escape) continue;
            } else if (!router.captured() && (overSurface || ed.surfaceDragging)) continue;
            router.Dispatch(ev);
        }
        if (!held) ed.surfaceDragging = false;
        ed.DrawMenu(quit);
        ed.DrawUi(quit);
        surfaceInput.focused = ed.surfaceFocused;
        render::UiFramePtr surfaceFrame;
        if (ed.surfaceEnabled && !ed.surfacePanel.empty()) {
            if (auto rendered = ed.surface->Render(ed.sco, ed.surfacePanel, 512, 384,
                    io.DeltaTime, surfaceInput, surfaceFrame); !rendered) {
                ed.surfaceStatus = rendered.error;
                log::Error("ui", "plugin surface: " + rendered.error);
                ed.surfaceEnabled = false;
                ed.surfaceFocused = false;
            } else ed.surfaceStatus.clear();
        }
        // Finish the surface before the main frame's texture list is handed to the renderer.
        render::UiFramePtr uiFrame = ed.ui.EndFrame();

        render::FramePacket packet;
        const render::Rect vp = ed.ui.viewport();
        packet.view.viewport = vp;
        packet.view.view = ed.camera.View();
        packet.view.projection = ed.camera.Projection(vp.w / std::max(vp.h, 1.0f));
        packet.view.eye = ed.camera.Eye();
        for (const auto& inst : ed.instances) {
            if (!inst.gpu) continue;
            packet.items.push_back({inst.gpu, inst.world, inst.material, inst.nodeId == ed.selectedNode});
        }
        if (ed.placement.active && ed.placement.hasPreview && ed.placement.gpu) {
            render::Material previewMaterial;
            previewMaterial.baseColor = {0.35f, 0.8f, 1.0f, 0.65f};
            packet.items.push_back({ed.placement.gpu, ed.placement.world, previewMaterial, false, true});
        }
        if (ed.surfaceEnabled && surfaceFrame) {
            render::PanelDraw pd;
            pd.panel = panel;
            pd.world = ed.SurfaceWorld();
            pd.ui = std::move(surfaceFrame);
            packet.panels.push_back(std::move(pd));
        }
        packet.ui = std::move(uiFrame);
        if (!window.minimized()) ed.renderer->Submit(std::move(packet));

        if (args.frames && ++frame >= args.frames) break;
    }

    if (args.frames) {
        ed.renderer->Flush();
        ed.PumpLoader();
        for (const auto& inst : ed.instances)
            if (!inst.gpu || !ed.renderer->Resident(inst.gpu)) {
                log::Error("app", inst.name + " did not become resident");
                code = 1;
            }
        if (gErrors.load()) code = 1;
        if (!args.readback.empty()) {
            std::vector<uint8_t> rgba;
            uint32_t w = 0, h = 0;
            if (auto rb = ed.renderer->Readback(rgba, w, h); !rb) {
                log::Error("render", "readback: " + rb.error);
                code = 1;
            } else {
                // Binary PPM: no encoder, no dependency, and every viewer reads it.
                std::FILE* out = std::fopen(args.readback.string().c_str(), "wb");
                if (!out) {
                    log::Error("render", "cannot write " + args.readback.generic_string());
                    code = 1;
                } else {
                    std::fprintf(out, "P6\n%u %u\n255\n", w, h);
                    std::vector<uint8_t> rgb(size_t{w} * h * 3);
                    size_t clearLikePixels = 0;
                    for (size_t i = 0; i < size_t{w} * h; ++i) {
                        const uint8_t r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
                        rgb[i * 3 + 0] = r;
                        rgb[i * 3 + 1] = g;
                        rgb[i * 3 + 2] = b;
                        // Descriptive only: plugin content normally covers the surface clear.
                        // Similar-coloured scene/editor pixels are not evidence of panel rendering.
                        if (std::abs(int{r} - 158) <= 8 && std::abs(int{g} - 33) <= 8 && std::abs(int{b} - 122) <= 8)
                            ++clearLikePixels;
                    }
                    std::fwrite(rgb.data(), 1, rgb.size(), out);
                    std::fclose(out);
                    std::fprintf(stderr, "scuba-engine: readback %s (%ux%u), clear-like pixels %zu (descriptive only)\n",
                                 args.readback.generic_string().c_str(), w, h, clearLikePixels);
                }
            }
        }
        std::fprintf(stderr, "scuba-engine: %d frame(s), %zu instance(s), %d error(s) -> exit %d\n", frame, ed.instances.size(), gErrors.load(), code);
    }

    if (ed.documentDirty.dirty())
        log::Warn("scene", std::string(ed.projectStore ? "unsaved scene changes were not saved: "
                                                      : "in-memory scene changes were discarded: ") + ed.document.id);
    if (ed.netSandbox && !ed.netSandbox->Shutdown())
        log::Warn("netsandbox", "a peer did not exit cleanly; child termination was confirmed");
    ed.netSandbox.reset();
    // Checked core shutdown runs while the host stack, UI, renderer and staged
    // plugin files still exist. A retained result terminates before teardown.
#ifdef SCUBA_CORE_LIFECYCLE_CANDIDATE
    if (!ed.sco.StopChecked()) std::_Exit(EXIT_FAILURE);
#else
    ed.sco.Stop();
#endif
    ed.renderer->Flush();
    ed.renderer->ReleasePanel(panel);
    ed.surface.reset();
    ed.ui.Shutdown();
    ed.loader.reset();   // before the job pool
    ed.renderer.reset();
    log::SetSink(nullptr);
    return code;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Log to an inherited stderr (pipe or file, e.g. under ctest); else to the parent's console.
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    const bool redirected = err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN;
    if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    const auto args = ParseArgs();
    if (!args) {
        std::fprintf(stderr, "usage: scuba-engine [--content DIR] [--mount PATH[=MOUNTPOINT][@PRIORITY]]... [--plugins DIR|--no-plugins] "
                             "[--data DIR] [--product DIR] [--open VFS_PATH]... [--warp] [--debug-layer] [--no-vsync] [--hidden] [--frames N] [--scenario FILE] [--diegetic [PANEL_ID]] [--readback FILE] [--export DIR | --export-pack FILE]\n");
        return 2;
    }
    return Run(*args);
}
