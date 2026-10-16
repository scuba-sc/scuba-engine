// SPDX-License-Identifier: GPL-3.0-only
// scuba-engine: the native editor. One main thread (window, sco-core game thread, UI, scene), a
// render thread inside the renderer, and a job pool for asset intake.
//
//   scuba-engine [--content DIR] [--mount PATH[=MOUNTPOINT][@PRIORITY]]... [--plugins DIR | --no-plugins]
//                [--data DIR] [--open VFS_PATH]... [--warp] [--debug-layer] [--no-vsync]
//                [--hidden] [--frames N]
//
// --frames N renders N frames and exits: 0 when every --open model became resident and nothing
// was logged at error level, 1 otherwise (the smoke test uses it with --warp --hidden).
#include "scuba/asset/model_loader.h"
#include "scuba/core/job_system.h"
#include "scuba/core/log.h"
#include "scuba/platform/window.h"
#include "scuba/render/camera.h"
#include "scuba/render/picking.h"
#include "scuba/render/renderer.h"
#include "scuba/sco/sco_host.h"
#include "scuba/ui/input_router.h"
#include "scuba/ui/ui_layer.h"
#include "scuba/vfs/pack.h"
#include "scuba/vfs/virtual_file_system.h"

#include "imgui.h"
#include "sco/widgets_imgui.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
    struct MountArg { std::string path, mountPoint; int priority = 10; };
    std::vector<MountArg> mounts;
    std::vector<std::string> open;
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
        if (k == "--content") { if (auto v = next()) args.content = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--plugins") { if (auto v = next()) args.plugins = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--no-plugins") args.plugins_on = false;
        else if (k == "--data") { if (auto v = next()) args.data = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--open") { if (auto v = next()) args.open.push_back(*v); else return std::nullopt; }
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
         showLoader = true, showLog = true, showStats = true, showDemo = false;
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

void Editor::DrawUi() {
    if (showContent) DrawContent();
    if (showMounts) DrawMounts();
    if (showScene) DrawScene();
    if (showInspector) DrawInspector();
    if (showPlugins) DrawPlugins();
    if (showLoader) DrawLoader();
    if (showStats) DrawStats();
    if (showLog) DrawLog();
    if (showDemo) ImGui::ShowDemoWindow(&showDemo);
}

int Run(const Args& args) {
    log::SetSink(&LogSink);
    platform::Window window;
    platform::WindowDesc wd;
    wd.title = "scuba-engine " SCUBA_VERSION;
    wd.visible = !args.hidden;
    if (auto st = window.Create(wd); !st) { log::Error("app", st.error); return 1; }

    Editor ed;
    render::RendererDesc rd;
    rd.window = window.native();
    rd.width = window.width();
    rd.height = window.height();
    rd.vsync = args.vsync;
    rd.debugLayer = args.debugLayer;
    rd.warp = args.warp;
    Status st;
    ed.renderer = render::Renderer::CreateD3D12(rd, st);
    if (!ed.renderer) { log::Error("render", st.error); return 1; }
    if (st = ed.ui.Init(window, *ed.renderer); !st) { log::Error("ui", st.error); return 1; }

    if (!args.content.empty() && fs::is_directory(args.content))
        if (auto m = MountPath(ed.vfs, args.content, "", 0); !m) log::Warn("vfs", m.error);
    for (const auto& m : args.mounts)
        if (auto r = MountPath(ed.vfs, PathFromUtf8(m.path), m.mountPoint, m.priority); !r) log::Error("vfs", r.error);
    ed.loader = std::make_unique<asset::ModelLoader>(ed.vfs, ed.jobs);

    ScoHostOptions so;
    if (args.plugins_on) so.pluginRoot = args.plugins;
    so.dataRoot = args.data;
    if (auto r = ed.sco.Start(so); !r) log::Warn("sco", r.error);   // the editor still runs without plugins

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
        packet.ui = std::move(uiFrame);
        if (!window.minimized()) ed.renderer->Submit(std::move(packet));

        if (args.frames && ++frame >= args.frames) break;
    }

    int code = 0;
    if (args.frames) {
        ed.renderer->Flush();
        ed.PumpLoader();
        for (const auto& inst : ed.scene)
            if (!inst.gpu || !ed.renderer->Resident(inst.gpu)) {
                log::Error("app", inst.name + " did not become resident");
                code = 1;
            }
        if (gErrors.load()) code = 1;
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
                             "[--data DIR] [--open VFS_PATH]... [--warp] [--debug-layer] [--no-vsync] [--hidden] [--frames N]\n");
        return 2;
    }
    return Run(*args);
}
