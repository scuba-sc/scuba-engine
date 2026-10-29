// SPDX-License-Identifier: GPL-3.0-only
// scuba-engine: the native editor. One main thread (window, sco-core game thread, UI, scene), a
// render thread inside the renderer, and a job pool for asset intake.
//
//   scuba-engine [--content DIR] [--mount PATH[=MOUNTPOINT][@PRIORITY]]... [--plugins DIR | --no-plugins]
//                [--data DIR] [--product DIR] [--open VFS_PATH]... [--warp] [--debug-layer] [--no-vsync]
//                [--hidden] [--frames N] [--scenario FILE] [--diegetic] [--readback FILE]
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
// --diegetic draws a render-to-texture panel as a world-space quad in front of the camera; with
// --readback FILE the frame is copied back and written as a binary PPM, and the run fails when the
// panel's own clear colour never reached the target (a panel nobody drew is magenta, so a missing
// quad cannot pass as a quiet frame).
//
// --export DIR / --export-pack FILE write a product distribution from the mounted --product root
// and exit without creating a window: <DIR>/data (the roster included) or one .scpak of the same
// tree. Both refuse an occupied destination and a roster folder without a plugin.ini.
#include "scuba/asset/model_loader.h"
#include "scuba/core/job_system.h"
#include "scuba/core/log.h"
#include "scuba/platform/window.h"
#include "scuba/render/camera.h"
#include "scuba/product/export.h"
#include "scuba/render/picking.h"
#include "scuba/render/renderer.h"
#include "scuba/sco/sco_host.h"
#include "scuba/ui/input_router.h"
#include "scuba/ui/ui_layer.h"
#include "scuba/vfs/pack.h"
#include "scuba/vfs/virtual_file_system.h"

#include "hostd/scenario_runner.h"

#include "imgui.h"
#include "sco/widgets_imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
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
    bool contentSet = false, pluginsSet = false, dataSet = false;
    struct MountArg { std::string path, mountPoint; int priority = 10; };
    std::vector<MountArg> mounts;
    std::vector<std::string> open;
    fs::path scenario;
    fs::path exportDir, exportPack;
    fs::path readback;
    bool diegetic = false;
    bool warp = false, debugLayer = false, vsync = true, hidden = false, plugins_on = true;
    int frames = 0;
};

std::string Utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
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
        else if (k == "--open") { if (auto v = next()) args.open.push_back(*v); else return std::nullopt; }
        else if (k == "--scenario") { if (auto v = next()) args.scenario = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--export") { if (auto v = next()) args.exportDir = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--export-pack") { if (auto v = next()) args.exportPack = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--readback") { if (auto v = next()) args.readback = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--diegetic") args.diegetic = true;
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
    // An export is a product distribution or nothing: it reads the mounted root, so it needs one,
    // and the two destinations are different artifacts.
    if (!args.exportDir.empty() && !args.exportPack.empty()) return std::nullopt;
    if ((!args.exportDir.empty() || !args.exportPack.empty()) && args.product.empty()) return std::nullopt;
    return args;
}

// Mounts a directory, .zip or .scpak by what the path is.
Status MountPath(vfs::VirtualFileSystem& vfs, const fs::path& path, const std::string& mountPoint, int priority) {
    Status st;
    std::unique_ptr<vfs::FileSystem> backend;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (fs::is_directory(path)) backend = vfs::OpenDirectory(path, st);
    else if (ext == ".zip") backend = vfs::OpenZip(path, st);
    else if (ext == ".scpak") backend = vfs::OpenPack(path, st);
    else return Status::Fail("not a directory, .zip or .scpak: " + path.generic_string());
    if (!backend) return st;
    vfs.Mount(std::move(backend), {mountPoint, priority, path.filename().generic_string()}, &st);
    if (st) log::Info("vfs", "mounted " + path.generic_string() + (mountPoint.empty() ? "" : " at " + mountPoint) + " (priority " + std::to_string(priority) + ")");
    return st;
}

// ---- scene -------------------------------------------------------------------------------------

struct Instance {
    uint32_t id = 0;
    std::string name;
    asset::ModelHandle handle = 0;
    render::GpuModel gpu = 0;
    std::shared_ptr<const asset::ModelData> model;
    render::Transform transform;
    render::Material material;
};

struct Editor {
    vfs::VirtualFileSystem vfs;
    JobSystem jobs;
    std::unique_ptr<asset::ModelLoader> loader;
    std::unique_ptr<render::Renderer> renderer;
    ScoHost sco;
    ui::UiLayer ui;
    render::OrbitCamera camera;

    std::vector<Instance> scene;
    uint32_t nextInstance = 1;
    uint32_t selected = 0;
    std::map<asset::ModelHandle, render::GpuModel> uploaded;   // one GPU copy per loaded model
    std::vector<asset::ModelHandle> requested;
    bool frameOnLoad = true;

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
    uint64_t logSeen = 0;
    std::vector<log::Line> logLines;

    Instance* Find(uint32_t id) {
        for (auto& i : scene) if (i.id == id) return &i;
        return nullptr;
    }

    void Spawn(const std::string& path) {
        const asset::ModelHandle h = loader->Request(path, JobPriority::High);
        requested.push_back(h);
        Instance inst;
        inst.id = nextInstance++;
        inst.name = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        inst.handle = h;
        // Spread new instances along +X so repeated spawns don't overlap.
        inst.transform.position = {static_cast<float>(scene.size()) * 2.5f, 0, 0};
        scene.push_back(inst);
        selected = inst.id;
    }

    // Finished loads: upload once per model, attach to the instances waiting for it.
    void PumpLoader() {
        for (asset::ModelHandle h : loader->TakeCompleted()) {
            const auto info = loader->Info(h);
            auto model = loader->Get(h);
            if (!model) {
                log::Error("assets", info.path + ": " + info.error);
                continue;
            }
            const render::GpuModel gpu = renderer->Upload(model);
            uploaded[h] = gpu;
            for (auto& inst : scene)
                if (inst.handle == h) {
                    inst.gpu = gpu;
                    inst.model = model;
                    // Sit the model on the ground plane.
                    if (inst.transform.position.z == 0) inst.transform.position.z = -model->boundsMin[2] * inst.transform.scale;
                    if (frameOnLoad) FrameInstance(inst);
                }
            frameOnLoad = false;
        }
        for (auto& inst : scene)
            if (!inst.gpu)
                if (auto it = uploaded.find(inst.handle); it != uploaded.end()) {
                    inst.gpu = it->second;
                    inst.model = loader->Get(inst.handle);
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
        auto mn = inst.model->boundsMin, mx = inst.model->boundsMax;
        for (int d = 0; d < 3; ++d) {
            mn[d] *= inst.transform.scale;
            mx[d] *= inst.transform.scale;
        }
        mn[0] += inst.transform.position.x; mx[0] += inst.transform.position.x;
        mn[1] += inst.transform.position.y; mx[1] += inst.transform.position.y;
        mn[2] += inst.transform.position.z; mx[2] += inst.transform.position.z;
        camera.Frame(mn, mx);
    }

    void Remove(uint32_t id) {
        scene.erase(std::remove_if(scene.begin(), scene.end(), [id](const Instance& i) { return i.id == id; }), scene.end());
        if (selected == id) selected = 0;
    }

    std::vector<render::PickTarget> PickTargets() const {
        std::vector<render::PickTarget> t;
        for (const auto& i : scene) if (i.model) t.push_back({i.id, i.model.get(), i.transform});
        return t;
    }

    void DrawUi();
    void DrawMenu(bool& quit);
    void DrawContent();
    void DrawMounts();
    void DrawScene();
    void DrawInspector();
    void DrawPlugins();
    void DrawLoader();
    void DrawStats();
    void DrawLog();
    void DrawScenario();
    void DrawProduct();
};

// ---- input layers --------------------------------------------------------------------------------

class UiBlocker final : public ui::InputLayer {
public:
    explicit UiBlocker(Editor& e) : e_(e) {}
    const char* Name() const override { return "ui"; }
    ui::Reply OnInput(const ui::InputEvent& ev) override {
        if (ev.kind == ui::InputKind::KeyDown) return e_.ui.keyboardToUi() ? ui::Reply::Handled() : ui::Reply::Unhandled();
        if (e_.ui.pointerOverUi() || !e_.ui.viewport().Contains(ev.x, ev.y)) return ui::Reply::Handled();
        return ui::Reply::Unhandled();
    }

private:
    Editor& e_;
};

// Ctrl + left drag moves the selection across its ground plane.
class MoveTool final : public ui::InputLayer {
public:
    explicit MoveTool(Editor& e) : e_(e) {}
    const char* Name() const override { return "move"; }
    ui::Reply OnInput(const ui::InputEvent& ev) override {
        Instance* sel = e_.Find(e_.selected);
        if (!sel) return ui::Reply::Unhandled();
        const auto ray = e_.camera.ScreenRay(ev.x, ev.y, e_.ui.viewport());
        const auto hit = render::RayPlaneZ(ray, sel->transform.position.z);
        if (ev.kind == ui::InputKind::PointerDown && ev.button == ui::PointerButton::Left && ev.ctrl && hit) {
            grab_ = {hit->x - sel->transform.position.x, hit->y - sel->transform.position.y, 0};
            return ui::Reply::Capture();
        }
        if (ev.kind == ui::InputKind::PointerMove && ev.leftHeld && hit) {
            sel->transform.position.x = hit->x - grab_.x;
            sel->transform.position.y = hit->y - grab_.y;
            return ui::Reply::Handled();
        }
        if (ev.kind == ui::InputKind::PointerUp && ev.button == ui::PointerButton::Left) return ui::Reply::Release();
        return ui::Reply::Unhandled();
    }

private:
    Editor& e_;
    render::Float3 grab_;
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
                e_.selected = hit ? hit->id : 0;
            }
            return ui::Reply::Release();
        case ui::InputKind::Wheel:
            e_.camera.Dolly(ev.wheel);
            return ui::Reply::Handled();
        case ui::InputKind::KeyDown:
            if (ev.key == ImGuiKey_F) { if (auto* s = e_.Find(e_.selected)) e_.FrameInstance(*s); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Delete && e_.selected) { e_.Remove(e_.selected); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Escape) { e_.selected = 0; return ui::Reply::Handled(); }
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
        if (ImGui::MenuItem("Clear scene")) { scene.clear(); selected = 0; }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) quit = true;
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
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter models", contentFilter, sizeof contentFilter);
    const std::string filter = vfs::NormalizePath(contentFilter);
    if (ImGui::BeginChild("files")) {
        for (const auto& e : vfs.List()) {
            if (asset::FormatOf(e.path) == asset::ModelFormat::Unknown) continue;
            if (!filter.empty() && e.path.find(filter) == std::string::npos) continue;
            ImGui::PushID(e.path.c_str());
            if (ImGui::Selectable(e.path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) Spawn(e.path);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%llu bytes\nfrom %s\ndouble-click to place", e.path.c_str(),
                                                          static_cast<unsigned long long>(e.size), vfs.Resolve(e.path).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
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
        if (drop) vfs.Unmount(*drop);
    }
    ImGui::SeparatorText("Mount");
    ImGui::InputTextWithHint("path", "directory, .zip or .scpak", mountPath, sizeof mountPath);
    ImGui::InputTextWithHint("mount point", "/ (root)", mountPoint, sizeof mountPoint);
    ImGui::InputInt("priority", &mountPriority);
    if (ImGui::Button("Mount")) {
        if (auto st = MountPath(vfs, PathFromUtf8(mountPath), mountPoint, mountPriority); !st) log::Error("vfs", st.error);
    }
    ImGui::End();
}

void Editor::DrawScene() {
    if (!ImGui::Begin("Scene", &showScene)) { ImGui::End(); return; }
    for (const auto& inst : scene) {
        const std::string label = inst.name + (inst.gpu ? "" : "  (loading)") + "##" + std::to_string(inst.id);
        if (ImGui::Selectable(label.c_str(), selected == inst.id)) selected = inst.id;
    }
    ImGui::End();
}

void Editor::DrawInspector() {
    if (!ImGui::Begin("Inspector", &showInspector)) { ImGui::End(); return; }
    Instance* s = Find(selected);
    if (!s) {
        ImGui::TextDisabled("Nothing selected");
        ImGui::End();
        return;
    }
    ImGui::Text("%s", s->name.c_str());
    if (s->model) ImGui::TextDisabled("%s  %zu mesh(es)  %zu triangles", s->model->path.c_str(), s->model->meshes.size(), s->model->triangleCount());
    ImGui::SeparatorText("Transform");
    ImGui::DragFloat3("position", &s->transform.position.x, 0.05f);
    float yawDeg = s->transform.yaw * 57.29578f;
    if (ImGui::DragFloat("yaw", &yawDeg, 0.5f)) s->transform.yaw = yawDeg / 57.29578f;
    ImGui::DragFloat("scale", &s->transform.scale, 0.01f, 0.001f, 1000.0f);
    ImGui::SeparatorText("Material (metallic-roughness)");
    ImGui::ColorEdit3("base color", &s->material.baseColor.x);
    ImGui::SliderFloat("metallic", &s->material.metallic, 0, 1);
    ImGui::SliderFloat("roughness", &s->material.roughness, 0.04f, 1);
    if (ImGui::Button("Frame (F)")) FrameInstance(*s);
    ImGui::SameLine();
    if (ImGui::Button("Remove (Del)")) Remove(s->id);
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

void Editor::DrawUi() {
    if (showContent) DrawContent();
    if (showMounts) DrawMounts();
    if (showScene) DrawScene();
    if (showInspector) DrawInspector();
    if (showPlugins) DrawPlugins();
    if (showLoader) DrawLoader();
    if (showStats) DrawStats();
    if (showLog) DrawLog();
    if (showScenario) DrawScenario();
    if (showProduct) DrawProduct();
    if (showDemo) ImGui::ShowDemoWindow(&showDemo);
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

    if (!content.empty() && fs::is_directory(content))
        if (auto m = MountPath(ed.vfs, content, "", 0); !m) log::Warn("vfs", m.error);
    for (const auto& m : args.mounts)
        if (auto r = MountPath(ed.vfs, PathFromUtf8(m.path), m.mountPoint, m.priority); !r) log::Error("vfs", r.error);
    ed.loader = std::make_unique<asset::ModelLoader>(ed.vfs, ed.jobs);

    ScoHostOptions so;
    if (args.plugins_on && args.product.empty()) so.pluginRoot = args.plugins;
    so.productRoot = args.product;
    so.dataRoot = data;
    if (auto r = ed.sco.Start(so); !r) {
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

    // The diegetic panel: one surface, filled with this frame's UI and drawn as a quad in front of
    // the camera. Created before the first frame so its descriptors are never written mid-flight.
    render::GpuPanel panel = 0;
    if (args.diegetic) {
        panel = ed.renderer->CreatePanel(512, 384);
        if (!panel) {
            log::Error("render", "diegetic panel creation failed");
            return 1;
        }
    }

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
    MoveTool move(ed);
    CameraTool cameraTool(ed);
    ui::InputRouter router;
    router.Add(&blocker);
    router.Add(&move);
    router.Add(&cameraTool);

    bool quit = false;
    int frame = 0;
    while (!quit && window.Pump()) {
        if (window.TakeResized()) ed.renderer->Resize(window.width(), window.height());
        ed.sco.Tick();
        ed.PumpLoader();

        ed.ui.BeginFrame();
        for (const auto& ev : ed.ui.Events()) router.Dispatch(ev);
        ed.DrawMenu(quit);
        ed.DrawUi();
        render::UiFramePtr uiFrame = ed.ui.EndFrame();

        render::FramePacket packet;
        const render::Rect vp = ed.ui.viewport();
        packet.view.viewport = vp;
        packet.view.view = ed.camera.View();
        packet.view.projection = ed.camera.Projection(vp.w / std::max(vp.h, 1.0f));
        packet.view.eye = ed.camera.Eye();
        for (const auto& inst : ed.scene) {
            if (!inst.gpu) continue;
            packet.items.push_back({inst.gpu, inst.transform.Matrix(), inst.material, inst.id == ed.selected});
        }
        if (panel) {
            // Rows of the view matrix are the camera's right, up and forward in world space: the quad
            // is built from them so it faces the camera wherever the camera is.
            const render::Float4x4& view = packet.view.view;
            const float w = 2.0f, h = 1.5f, distance = 3.0f;
            render::PanelDraw pd;
            pd.panel = panel;
            // The shader multiplies a row vector by this matrix, so the rows are the quad's axes:
            // right, up and forward from the view matrix, then the centre in the last row.
            for (int c = 0; c < 4; ++c) {
                pd.world.m[0][c] = view.m[0][c] * w;
                pd.world.m[1][c] = view.m[1][c] * h;
                pd.world.m[2][c] = view.m[2][c];
            }
            // Row 2 of a D3D view matrix points back towards the camera, so it is both the quad's
            // normal (the quad faces +Z) and the direction the centre is offset against.
            const render::Float3 eye = packet.view.eye;
            const render::Float3 back{view.m[2][0], view.m[2][1], view.m[2][2]};
            pd.world.m[3][0] = eye.x - back.x * distance;
            pd.world.m[3][1] = eye.y - back.y * distance;
            pd.world.m[3][2] = eye.z - back.z * distance;
            pd.world.m[3][3] = 1.0f;
            packet.panels.push_back(pd);
        }
        packet.ui = std::move(uiFrame);
        if (!window.minimized()) ed.renderer->Submit(std::move(packet));

        if (args.frames && ++frame >= args.frames) break;
    }

    if (args.frames) {
        ed.renderer->Flush();
        ed.PumpLoader();
        for (const auto& inst : ed.scene)
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
                    size_t panelPixels = 0;
                    for (size_t i = 0; i < size_t{w} * h; ++i) {
                        const uint8_t r = rgba[i * 4 + 0], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
                        rgb[i * 3 + 0] = r;
                        rgb[i * 3 + 1] = g;
                        rgb[i * 3 + 2] = b;
                        // The panel's clear colour, which only the panel surface and the quad that
                        // samples it can put on screen.
                        if (std::abs(int{r} - 158) <= 8 && std::abs(int{g} - 33) <= 8 && std::abs(int{b} - 122) <= 8)
                            ++panelPixels;
                    }
                    std::fwrite(rgb.data(), 1, rgb.size(), out);
                    std::fclose(out);
                    std::fprintf(stderr, "scuba-engine: readback %s (%ux%u), panel pixels %zu\n",
                                 args.readback.generic_string().c_str(), w, h, panelPixels);
                    if (panelPixels < 64) {
                        log::Error("render", "diegetic panel never reached the frame: " + std::to_string(panelPixels) +
                                                 " panel pixel(s)");
                        code = 1;
                    }
                }
            }
        }
        std::fprintf(stderr, "scuba-engine: %d frame(s), %zu instance(s), %d error(s) -> exit %d\n", frame, ed.scene.size(), gErrors.load(), code);
    }

    ed.renderer->Flush();
    ed.ui.Shutdown();
    ed.sco.Stop();
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
                             "[--data DIR] [--product DIR] [--open VFS_PATH]... [--warp] [--debug-layer] [--no-vsync] [--hidden] [--frames N] [--scenario FILE] [--diegetic] [--readback FILE] [--export DIR | --export-pack FILE]\n");
        return 2;
    }
    return Run(*args);
}
