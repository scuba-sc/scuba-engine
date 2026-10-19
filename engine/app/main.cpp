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
#include "scuba/core/png.h"
#include "scuba/platform/window.h"
#include "scuba/render/camera.h"
#include "scuba/render/picking.h"
#include "scuba/render/renderer.h"
#include "scuba/sco/sco_host.h"
#include "scuba/ui/input_router.h"
#include "scuba/ui/ui_layer.h"
#include "scuba/ui/plugin_designer.h"
#include "scuba/vfs/pack.h"
#include "scuba/vfs/virtual_file_system.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "sco/widgets_imgui.h"
#include "sco/runtime.h"
#include "hostd/server.h"
#include "hostd/modules.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    fs::path productRoot;
    fs::path data;
    struct MountArg { std::string path, mountPoint; int priority = 10; };
    std::vector<MountArg> mounts;
    std::vector<std::string> open;
    bool warp = false, debugLayer = false, vsync = true, hidden = false, plugins_on = true;
    int frames = 0;
    std::optional<uint16_t> controlPort;
    fs::path drive;
    uint32_t width = 1600, height = 900;
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
        else if (k == "--product-profile") { if (auto v = next()) args.productRoot = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--no-plugins") args.plugins_on = false;
        else if (k == "--data") { if (auto v = next()) args.data = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--control-port") {
            const auto value = next();
            if (!value) return std::nullopt;
            uint32_t port = 0;
            const auto parsed = std::from_chars(value->data(), value->data() + value->size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || port > 65535) return std::nullopt;
            args.controlPort = static_cast<uint16_t>(port);
        }
        else if (k == "--no-control-port") args.controlPort.reset();
        else if (k == "--drive") { if (auto v = next()) args.drive = PathFromUtf8(*v); else return std::nullopt; }
        else if (k == "--width" || k == "--height") {
            const auto value = next();
            if (!value) return std::nullopt;
            uint32_t extent = 0;
            const auto parsed = std::from_chars(value->data(), value->data() + value->size(), extent);
            if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || extent < 640 || extent > 4096) return std::nullopt;
            (k == "--width" ? args.width : args.height) = extent;
        }
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
Status MountPath(vfs::VirtualFileSystem& vfs, const fs::path& path, const std::string& mountPoint, int priority, vfs::MountId* id = nullptr) {
    Status st;
    std::unique_ptr<vfs::FileSystem> backend;
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    if (fs::is_directory(path)) backend = vfs::OpenDirectory(path, st);
    else if (ext == ".zip") backend = vfs::OpenZip(path, st);
    else if (ext == ".scpak") backend = vfs::OpenPack(path, st);
    else return Status::Fail("not a directory, .zip or .scpak: " + path.generic_string());
    if (!backend) return st;
    const auto mountId = vfs.Mount(std::move(backend), {mountPoint, priority, path.filename().generic_string()}, &st);
    if (st) log::Info("vfs", "mounted " + path.generic_string() + (mountPoint.empty() ? "" : " at " + mountPoint) + " (priority " + std::to_string(priority) + ")");
    if (id) *id = mountId;
    return st;
}

// Scene files are studio documents: <data-root>/scenes/<name>.json, schema 1.
bool BoundsOk(const hostd::Json& item, std::string* why) {
    const double numbers[] = {0, 0, 0, 0, 0.001, 0, 0, 0, 0, 0, 0.04};
    const char* keys[] = {"x", "y", "z", "yaw", "scale", "red", "green", "blue", "alpha", "metallic", "roughness"};
    for (int i = 0; i < 11; ++i) {
        const auto* value = item.find(keys[i]);
        if (!value || !value->is_number()) { *why = std::string("missing ") + keys[i]; return false; }
        const double v = value->as_number();
        if (!std::isfinite(v) || v < (numbers[i] == 0.0 ? -1000000.0 : numbers[i]) ||
            (i == 1 && std::fabs(v) > 1000000) || (i == 0 && std::fabs(v) > 1000000) || (i == 2 && std::fabs(v) > 1000000) ||
            (i == 3 && std::fabs(v) > static_cast<double>(3.141592653589793f)) ||
            (i == 4 && v > 1000) || ((i >= 5 && i <= 9) && v > 1) || (i == 10 && v > 1)) { *why = std::string(keys[i]) + " outside bounds"; return false; }
    }
    return true;
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
    ui::PluginDesigner designer;
    bool showDesigner = true;
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
    char productPath[512] = "";
    std::string productStatus;
    ScoHostOptions hostOptions;
    uint64_t logSeen = 0;
    std::vector<log::Line> logLines;
    void RegisterControls();
    void Select(uint32_t id) {
        sco::Arg value{}; value.type = sco::ArgType::Int; value.v.i = id;
        sco::Invoke("scene.select", &value, 1, nullptr, nullptr);
    }
    void TransformSelection(const render::Transform& transform) {
        sco::Arg values[5]{};
        const double numbers[] = {transform.position.x, transform.position.y, transform.position.z, transform.yaw, transform.scale};
        for (uint32_t i = 0; i < 5; ++i) { values[i].type = sco::ArgType::Float; values[i].v.f = numbers[i]; }
        sco::Invoke("scene.selection.transform", values, 5, nullptr, nullptr);
    }
    void MaterialSelection(const render::Material& material) {
        sco::Arg values[5]{};
        const double numbers[] = {material.baseColor.x, material.baseColor.y, material.baseColor.z, material.metallic, material.roughness};
        for (uint32_t i = 0; i < 5; ++i) { values[i].type = sco::ArgType::Float; values[i].v.f = numbers[i]; }
        sco::Invoke("scene.selection.material", values, 5, nullptr, nullptr);
    }
    void Place(const std::string& path) {
        sco::Arg value{}; value.type = sco::ArgType::String; value.v.s = path.c_str();
        sco::Invoke("scene.place", &value, 1, nullptr, nullptr);
    }

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
            auto transform = sel->transform;
            transform.position.x = hit->x - grab_.x;
            transform.position.y = hit->y - grab_.y;
            e_.TransformSelection(transform);
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
                e_.Select(hit ? hit->id : 0);
            }
            return ui::Reply::Release();
        case ui::InputKind::Wheel:
            e_.camera.Dolly(ev.wheel);
            return ui::Reply::Handled();
        case ui::InputKind::KeyDown:
            if (ev.key == ImGuiKey_F) { e_.sco.InvokeControl("scene.selection.frame"); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Delete && e_.selected) { e_.sco.InvokeControl("scene.selection.remove"); return ui::Reply::Handled(); }
            if (ev.key == ImGuiKey_Escape) { e_.Select(0); return ui::Reply::Handled(); }
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
        if (ImGui::MenuItem("Clear scene")) {
            const auto status = sco.InvokeControl("scene.clear");
            if (!status) log::Error("control", status.error);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Alt+F4")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Assets", nullptr, &showContent);
        ImGui::MenuItem("Mounts", nullptr, &showMounts);
        ImGui::MenuItem("Scene", nullptr, &showScene);
        ImGui::MenuItem("Inspector", nullptr, &showInspector);
        ImGui::MenuItem("Plugins", nullptr, &showPlugins);
        ImGui::MenuItem("Designer", nullptr, &showDesigner);
        ImGui::MenuItem("Asset loader", nullptr, &showLoader);
        ImGui::MenuItem("Stats", nullptr, &showStats);
        ImGui::MenuItem("Console", nullptr, &showLog);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset workspace")) {
            ui.ResetWorkspace();
            showContent = showMounts = showScene = showInspector = showPlugins = showLoader = showLog = showStats = true;
        }
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
    if (!ImGui::Begin("Assets", &showContent)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "filter models", contentFilter, sizeof contentFilter);
    const std::string filter = vfs::NormalizePath(contentFilter);
    if (ImGui::BeginChild("files")) {
        for (const auto& e : vfs.List()) {
            if (asset::FormatOf(e.path) == asset::ModelFormat::Unknown) continue;
            if (!filter.empty() && e.path.find(filter) == std::string::npos) continue;
            ImGui::PushID(e.path.c_str());
            if (ImGui::Selectable(e.path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) Place(e.path);
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
        if (ImGui::Selectable(label.c_str(), selected == inst.id)) Select(inst.id);
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
    ImGui::TextUnformatted(s->name.c_str());
    if (s->model) {
        ImGui::TextWrapped("%s", s->model->path.c_str());
        ImGui::TextDisabled("%zu meshes  /  %zu triangles", s->model->meshes.size(), s->model->triangleCount());
    }
    ImGui::PushItemWidth(-1);
    ImGui::SeparatorText("Transform");
    auto transform = s->transform;
    ImGui::TextDisabled("Position");
    bool changed = ImGui::DragFloat3("##position", &transform.position.x, 0.05f, -1000000.0f, 1000000.0f);
    float yawDeg = transform.yaw * 57.29578f;
    ImGui::TextDisabled("Yaw");
    if (ImGui::DragFloat("##yaw", &yawDeg, 0.5f, -180.0f, 180.0f)) {
        transform.yaw = yawDeg / 57.29578f; changed = true;
    }
    ImGui::TextDisabled("Scale");
    changed = ImGui::DragFloat("##scale", &transform.scale, 0.01f, 0.001f, 1000.0f) || changed;
    if (changed) TransformSelection(transform);
    ImGui::SeparatorText("Material (metallic-roughness)");
    auto material = s->material;
    ImGui::TextDisabled("Base color");
    bool materialChanged = ImGui::ColorEdit3("##base color", &material.baseColor.x);
    ImGui::TextDisabled("Metallic");
    materialChanged = ImGui::SliderFloat("##metallic", &material.metallic, 0, 1) || materialChanged;
    ImGui::TextDisabled("Roughness");
    materialChanged = ImGui::SliderFloat("##roughness", &material.roughness, 0.04f, 1) || materialChanged;
    if (materialChanged) MaterialSelection(material);
    ImGui::PopItemWidth();
    if (ImGui::Button("Frame (F)")) sco.InvokeControl("scene.selection.frame");
    ImGui::SameLine();
    if (ImGui::Button("Remove (Del)")) sco.InvokeControl("scene.selection.remove");
    ImGui::End();
}

void Editor::DrawPlugins() {
    static std::string selection;
    static std::string selectionStatus;
    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    static bool profileDocked = false;
    if (!profileDocked) {
        if (const auto* dock = ImGui::FindWindowByName("Plugins"); dock && dock->DockId) {
            ImGui::SetNextWindowDockID(dock->DockId, ImGuiCond_Always);
            profileDocked = true;
        }
    }
    if (ImGui::Begin("Product Inspector & Scenario Runner")) {
        ImGui::TextUnformatted("sc-offline product profile");
        ImGui::TextDisabled("Synthetic offline host; no game connection");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##product-root", productPath, sizeof(productPath));
        if (ImGui::Button("Load Product Profile (sc-offline)")) {
            const auto root = PathFromUtf8(productPath);
            if (!fs::is_directory(root / "data" / "plugins")) {
                productStatus = "Profile requires an existing data/plugins directory";
            } else {
                selection.clear();
                selectionStatus.clear();
                sco.Stop();
                hostOptions.productRoot = root;
                const auto status = sco.Start(hostOptions);
                if (status) RegisterControls();
                productStatus = status ? "Product profile loaded" : status.error;
            }
        }
        if (!productStatus.empty()) ImGui::TextWrapped("%s", productStatus.c_str());
        ImGui::Separator();
        ImGui::Text("Loaded folders: %zu; retained panels: %zu", sco.Plugins().size(), sco.Panels().size());
        ImGui::TextWrapped("Use Plugin controls to select and interact with every retained product panel.");
        if (ImGui::CollapsingHeader("Owner storage inventory")) {
            ImGui::TextWrapped("Read-only database file inventory. Sizes exclude WAL sidecars; no private keys or values are opened.");
            const auto files = sco.StorageFiles();
            if (files.empty()) ImGui::TextDisabled("No owner database files present");
            for (const auto& file : files)
                ImGui::TextWrapped("%s.db: %llu bytes", file.owner.c_str(), static_cast<unsigned long long>(file.bytes));
        }
        if (ImGui::CollapsingHeader("ASOP next-start selection")) {
            ImGui::TextWrapped("Copied committed selector snapshot. Saving does not synchronize live terminals.");
            if (ImGui::Button("Read committed ASOP selection")) {
                const auto status = sco.ReadAsopSelection(selection);
                selectionStatus = status ? (selection.empty() ? "Committed empty selection: no ships requested for next start" : "Committed selection copied") : status.error;
                if (!status) selection.clear();
            }
            ImGui::TextWrapped("%s", selectionStatus.c_str());
            if (!selection.empty()) ImGui::TextWrapped("%s", selection.c_str());
        }
        ImGui::SeparatorText("Synthetic scenarios");
        if (ImGui::Button("Admit mock capabilities"))
            productStatus = sco.InvokeControl("sco.mock.admit") ? "Synthetic capabilities admitted" : "Mock host unavailable";
        if (ImGui::Button("Revoke mock capabilities"))
            productStatus = sco.InvokeControl("sco.mock.revoke") ? "Synthetic capabilities revoked" : "Mock host unavailable";
        if (ImGui::CollapsingHeader("Capability admission readback")) {
            ImGui::TextWrapped("Synthetic capability switches only. This is not production G1 admission or in-game verification.");
            for (const auto& capability : sco.MockCapabilities())
                ImGui::TextWrapped("%s: %s", capability.name.c_str(), capability.ready ? "ready" : "unavailable");
        }
        if (ImGui::Button("RespawnLoadoutCompleted"))
            productStatus = sco.InvokeControl("sco.mock.fire_respawn_loadout") ? "Synthetic respawn occurrence dispatched" : "Event unavailable or refused";
        if (ImGui::Button("Mission changed (synthetic 1)"))
            productStatus = sco.InvokeControl("sco.mock.fire_mission_changed") ? "Synthetic mission occurrence dispatched" : "Event unavailable or refused";
        static char entityClass[96] = "Synthetic_Actor";
        static bool actor = true;
        ImGui::TextDisabled("Inject synthetic entity class");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##entity-class", entityClass, sizeof(entityClass));
        ImGui::Checkbox("Actor", &actor);
        if (ImGui::Button("Inject entity")) {
            const auto id = sco.AddMockEntity(entityClass, actor);
            productStatus = id ? "Synthetic entity id=" + std::to_string(id) : "Entity injection refused";
        }
        ImGui::Text("Synthetic entities: %zu", sco.MockEntityCount());
        ImGui::SeparatorText("Product distribution");
        static char exportPath[512] = "";
        ImGui::TextDisabled("New destination directory (outside source)");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##product-export", exportPath, sizeof(exportPath));
        if (ImGui::Button("Export product bundle + SHA256SUMS")) {
            const auto status = sco.ExportProduct(PathFromUtf8(exportPath));
            productStatus = status ? "Product bundle exported with SHA-256 inventory" : status.error;
        }
    }
    ImGui::End();
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

    // Keep retained SDK panels together; callbacks still pass through host validation.
    static sco::widgets::ImGuiRenderer widgets;
    if (ImGui::Begin("Plugin controls")) {
        const auto panels = sco.Panels();
        if (panels.empty()) ImGui::TextDisabled("No plugin panels published");
        if (ImGui::BeginTabBar("retained-panels")) {
            for (const auto& panel : panels) {
                const std::string title = (panel.title.empty() ? panel.id : panel.title) + "###panel:" + panel.id;
                if (ImGui::BeginTabItem(title.c_str())) {
                    ImGui::TextDisabled("%s", panel.owner.c_str());
                    ImGui::Separator();
                    ImGui::PushTextWrapPos(0.0f);
                    if (auto st = sco.RenderPanel(panel.id, widgets); !st) ImGui::TextWrapped("%s", st.error.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
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
    if (!ImGui::Begin("Console", &showLog)) { ImGui::End(); return; }
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
    const auto vp = ui.viewport();
    const auto origin = ImGui::GetMainViewport()->Pos;
    ImGui::SetNextWindowPos({origin.x + vp.x + 12, origin.y + vp.y + 12});
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("##viewport-toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove)) {
        ImGui::TextDisabled("Perspective"); ImGui::SameLine();
        ImGui::BeginDisabled(!Find(selected));
        if (ImGui::Button("Frame selection")) sco.InvokeControl("scene.selection.frame");
        ImGui::EndDisabled();
    }
    ImGui::End();
    if (showContent) DrawContent();
    if (showMounts) DrawMounts();
    if (showScene) DrawScene();
    if (showInspector) DrawInspector();
    if (showPlugins) DrawPlugins();
    if (showDesigner) designer.Draw(&showDesigner);
    if (showLoader) DrawLoader();
    if (showStats) DrawStats();
    if (showLog) DrawLog();
    if (showDemo) ImGui::ShowDemoWindow(&showDemo);
}

// Scene files are studio documents: <data-root>/scenes/<name>.json, schema 1.
static bool ScenePathFor(const Editor& editor, const std::string& name, std::filesystem::path* out, std::string* why) {
    if (editor.hostOptions.dataRoot.empty()) { *why = "Data root not set"; return false; }
    if (name.empty() || name.size() > 96 || name.find("..") != std::string::npos) { *why = "Scene name invalid"; return false; }
    for (const unsigned char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            *why = "Scene name invalid"; return false;
        }
    *out = editor.hostOptions.dataRoot / "scenes" / name;
    return true;
}

void Editor::RegisterControls() {
    const sco::ArgDef sceneNameArg{"name", sco::ArgType::String, "Scene file in the data root: 1-96 of [A-Za-z0-9._-], no .."};
    const sco::ArgDef args[] = {{"yaw", sco::ArgType::Float, "Radians, -pi..pi"},
        {"pitch", sco::ArgType::Float, "Radians, -1.5..1.5"},
        {"distance", sco::ArgType::Float, "Metres, 0.1..10000"}};
    const sco::Command command{"scene.camera.set", "Set editor camera", "Set the editor orbit camera, never the mock game camera",
        nullptr, args, 3, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            const double yaw = values[0].v.f, pitch = values[1].v.f, distance = values[2].v.f;
            if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(distance) ||
                yaw < -3.141592653589793 || yaw > 3.141592653589793 || pitch < -1.5 || pitch > 1.5 || distance < 0.1 || distance > 10000) {
                std::snprintf(reply, size, "Editor camera arguments outside bounds; unchanged"); return sco::Result::BadArg;
            }
            auto& camera = static_cast<Editor*>(context)->camera;
            camera.yaw = static_cast<float>(yaw); camera.pitch = static_cast<float>(pitch); camera.distance = static_cast<float>(distance);
            std::snprintf(reply, size, "Editor camera updated"); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, command) != sco::Result::Ok)
        log::Error("control", "Editor camera command registration refused");
    const sco::Command clear{"scene.clear", "Clear scene", "Remove all scene instances and selection; retain cached assets",
        nullptr, nullptr, 0, [](const sco::Arg*, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            editor.scene.clear(); editor.selected = 0;
            std::snprintf(reply, size, "Scene cleared"); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, clear) != sco::Result::Ok)
        log::Error("control", "Scene clear command registration refused");
    const sco::Command frame{"scene.selection.frame", "Frame selection", "Frame the selected loaded scene instance",
        nullptr, nullptr, 0, [](const sco::Arg*, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            const auto* item = editor.Find(editor.selected);
            if (!item || !item->model) {
                std::snprintf(reply, size, "No loaded selection; camera unchanged"); return sco::Result::BadArg;
            }
            editor.FrameInstance(*item);
            std::snprintf(reply, size, "Selection framed"); return sco::Result::Ok;
        }, this};
    const sco::Command remove{"scene.selection.remove", "Remove selection", "Remove the selected scene instance",
        nullptr, nullptr, 0, [](const sco::Arg*, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            if (!editor.Find(editor.selected)) {
                std::snprintf(reply, size, "No selection; scene unchanged"); return sco::Result::BadArg;
            }
            editor.Remove(editor.selected);
            std::snprintf(reply, size, "Selection removed"); return sco::Result::Ok;
        }, this};
    if (sco.started()) {
        if (sco::RegisterCommand(this, nullptr, frame) != sco::Result::Ok ||
            sco::RegisterCommand(this, nullptr, remove) != sco::Result::Ok)
            log::Error("control", "Scene selection command registration refused");
    }
    const sco::ArgDef selectArg{"id", sco::ArgType::Int, "Existing instance ID, or zero to deselect"};
    const sco::Command select{"scene.select", "Select instance", "Select an existing scene instance; zero clears selection",
        nullptr, &selectArg, 1, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            const int64_t id = values[0].v.i;
            if (id < 0 || id > UINT32_MAX || (id && !editor.Find(static_cast<uint32_t>(id)))) {
                std::snprintf(reply, size, "Unknown instance; selection unchanged"); return sco::Result::BadArg;
            }
            editor.selected = static_cast<uint32_t>(id);
            std::snprintf(reply, size, "Selection updated"); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, select) != sco::Result::Ok)
        log::Error("control", "Scene select command registration refused");
    const sco::ArgDef transformArgs[] = {{"x", sco::ArgType::Float, "Metres, -1000000..1000000"},
        {"y", sco::ArgType::Float, "Metres, -1000000..1000000"},
        {"z", sco::ArgType::Float, "Metres, -1000000..1000000"},
        {"yaw", sco::ArgType::Float, "Radians, -pi..pi"},
        {"scale", sco::ArgType::Float, "Uniform scale, 0.001..1000"}};
    const sco::Command transform{"scene.selection.transform", "Transform selection", "Atomically set selected instance position, yaw and uniform scale",
        nullptr, transformArgs, 5, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            auto* item = editor.Find(editor.selected);
            if (!item) {
                std::snprintf(reply, size, "No selection; transform unchanged"); return sco::Result::BadArg;
            }
            for (uint32_t i = 0; i < 5; ++i)
                if (!std::isfinite(values[i].v.f)) {
                    std::snprintf(reply, size, "Non-finite transform; unchanged"); return sco::Result::BadArg;
                }
            if (std::fabs(values[0].v.f) > 1000000 || std::fabs(values[1].v.f) > 1000000 ||
                std::fabs(values[2].v.f) > 1000000 || std::fabs(values[3].v.f) > static_cast<double>(3.141592653589793f) ||
                values[4].v.f < 0.001 || values[4].v.f > 1000) {
                std::snprintf(reply, size, "Transform outside bounds; unchanged"); return sco::Result::BadArg;
            }
            item->transform.position = {static_cast<float>(values[0].v.f), static_cast<float>(values[1].v.f), static_cast<float>(values[2].v.f)};
            item->transform.yaw = static_cast<float>(values[3].v.f);
            item->transform.scale = static_cast<float>(values[4].v.f);
            std::snprintf(reply, size, "Selection transformed"); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, transform) != sco::Result::Ok)
        log::Error("control", "Scene transform command registration refused");
    const sco::ArgDef materialArgs[] = {{"red", sco::ArgType::Float, "0..1"},
        {"green", sco::ArgType::Float, "0..1"}, {"blue", sco::ArgType::Float, "0..1"},
        {"metallic", sco::ArgType::Float, "0..1"}, {"roughness", sco::ArgType::Float, "0.04..1"}};
    const sco::Command material{"scene.selection.material", "Set selection material", "Atomically set selected instance RGB, metallic and roughness",
        nullptr, materialArgs, 5, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            auto* item = editor.Find(editor.selected);
            if (!item) {
                std::snprintf(reply, size, "No selection; material unchanged"); return sco::Result::BadArg;
            }
            for (uint32_t i = 0; i < 5; ++i)
                if (!std::isfinite(values[i].v.f) || values[i].v.f > 1 ||
                    values[i].v.f < (i == 4 ? static_cast<double>(0.04f) : 0.0)) {
                    std::snprintf(reply, size, "Material outside bounds; unchanged"); return sco::Result::BadArg;
                }
            item->material.baseColor.x = static_cast<float>(values[0].v.f);
            item->material.baseColor.y = static_cast<float>(values[1].v.f);
            item->material.baseColor.z = static_cast<float>(values[2].v.f);
            item->material.metallic = static_cast<float>(values[3].v.f);
            item->material.roughness = static_cast<float>(values[4].v.f);
            std::snprintf(reply, size, "Selection material updated"); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, material) != sco::Result::Ok)
        log::Error("control", "Scene material command registration refused");
    const sco::ArgDef pathArg{"path", sco::ArgType::String, "Existing mounted model VFS path, at most 4096 bytes"};
    const sco::Command place{"scene.place", "Place model", "Request a mounted model and add a scene instance; loading is asynchronous",
        nullptr, &pathArg, 1, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            const std::string path = values[0].v.s ? values[0].v.s : "";
            if (path.empty() || path.size() > 4096 || vfs::NormalizePath(path) != path ||
                asset::FormatOf(path) == asset::ModelFormat::Unknown || !editor.vfs.Exists(path) ||
                editor.scene.size() >= 4096 || editor.nextInstance == UINT32_MAX) {
                std::snprintf(reply, size, "Invalid model path or scene capacity; unchanged"); return sco::Result::BadArg;
            }
            editor.Spawn(path);
            std::snprintf(reply, size, "Instance %u requested; loading may be pending", editor.selected); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, place) != sco::Result::Ok)
        log::Error("control", "Scene placement command registration refused");
    const sco::ArgDef mountArgs[] = {{"path", sco::ArgType::String, "Existing directory, .zip or .scpak; at most 4096 bytes"},
        {"mount_point", sco::ArgType::String, "VFS prefix, empty for root"}, {"priority", sco::ArgType::Int, "-1000..1000"}};
    const sco::Command mount{"vfs.mount", "Mount archive", "Mount a directory, zip or scpak into the studio VFS; first matching path wins by priority",
        nullptr, mountArgs, 3, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            const std::string path = values[0].v.s ? values[0].v.s : "";
            const std::string mountPoint = values[1].v.s ? values[1].v.s : "";
            const int64_t priority = values[2].v.i;
            if (path.empty() || path.size() > 4096 || mountPoint.size() > 255 ||
                priority < -1000 || priority > 1000 ||
                vfs::NormalizePath(mountPoint) != mountPoint) {
                std::snprintf(reply, size, "Invalid mount arguments"); return sco::Result::BadArg;
            }
            std::error_code error;
            const auto source = PathFromUtf8(path);
            if (!fs::exists(source, error)) {
                std::snprintf(reply, size, "Mount source does not exist"); return sco::Result::Failed;
            }
            Status status;
            vfs::MountId newId = 0;
            if (status = MountPath(editor.vfs, source, mountPoint, static_cast<int>(priority), &newId); !status) {
                std::snprintf(reply, size, "%s", status.error.c_str()); return sco::Result::Failed;
            }
            std::snprintf(reply, size, "Mounted as id %u", newId);
            return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, mount) != sco::Result::Ok)
        log::Error("control", "VFS mount command registration refused");
    const sco::ArgDef unmountArg{"id", sco::ArgType::Int, "Existing VFS mount id"};
    const sco::Command unmount{"vfs.unmount", "Unmount archive", "Remove a studio VFS mount; scene instances keep the files they already opened",
        nullptr, &unmountArg, 1, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            const int64_t id = values[0].v.i;
            if (id < 1 || id > UINT32_MAX) {
                std::snprintf(reply, size, "Mount id out of range"); return sco::Result::BadArg;
            }
            if (!editor.vfs.Unmount(static_cast<vfs::MountId>(id))) {
                std::snprintf(reply, size, "Unknown mount; VFS unchanged"); return sco::Result::NotFound;
            }
            std::snprintf(reply, size, "Mount %lld unmounted", static_cast<long long>(id)); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, unmount) != sco::Result::Ok)
        log::Error("control", "VFS unmount command registration refused");
    const sco::Command save{"scene.save", "Save scene", "Write all scene instances to a new scene file under the data root",
        nullptr, &sceneNameArg, 1, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            std::filesystem::path destination;
            std::string why;
            if (!ScenePathFor(editor, values[0].v.s ? values[0].v.s : "", &destination, &why)) {
                std::snprintf(reply, size, "%s", why.c_str()); return sco::Result::BadArg;
            }
            hostd::Json doc = hostd::Json::MakeObject();
            doc.set("schema", 1);
            hostd::Json items = hostd::Json::MakeArray();
            for (const auto& instance : editor.scene) {
                hostd::Json item = hostd::Json::MakeObject();
                item.set("path", editor.loader->Info(instance.handle).path);
                item.set("x", instance.transform.position.x); item.set("y", instance.transform.position.y);
                item.set("z", instance.transform.position.z); item.set("yaw", instance.transform.yaw);
                item.set("scale", instance.transform.scale);
                item.set("red", instance.material.baseColor.x); item.set("green", instance.material.baseColor.y);
                item.set("blue", instance.material.baseColor.z); item.set("alpha", instance.material.baseColor.w);
                item.set("metallic", instance.material.metallic); item.set("roughness", instance.material.roughness);
                items.push_back(std::move(item));
            }
            doc.set("instances", std::move(items));
            std::error_code error;
            fs::create_directories(destination.parent_path(), error);
            FILE* file = nullptr;
            if (_wfopen_s(&file, destination.c_str(), L"wbx") != 0) file = nullptr;
            if (!file) {
                std::snprintf(reply, size, "Scene file exists or cannot be created"); return sco::Result::Failed;
            }
            const std::string text = doc.dump() + "\n";
            const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
            const bool closed = std::fclose(file) == 0;
            if (!written || !closed) {
                std::filesystem::remove(destination, error);
                std::snprintf(reply, size, "Scene write failed"); return sco::Result::Failed;
            }
            std::snprintf(reply, size, "Saved %zu instance(s)", editor.scene.size()); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, save) != sco::Result::Ok)
        log::Error("control", "Scene save command registration refused");
    const sco::Command load{"scene.load", "Load scene", "Validate a scene file, then rebuild the scene all-or-nothing; instances load asynchronously",
        nullptr, &sceneNameArg, 1, [](const sco::Arg* values, uint32_t, void* context, char* reply, uint32_t size) {
            auto& editor = *static_cast<Editor*>(context);
            std::filesystem::path source;
            std::string why;
            if (!ScenePathFor(editor, values[0].v.s ? values[0].v.s : "", &source, &why)) {
                std::snprintf(reply, size, "%s", why.c_str()); return sco::Result::BadArg;
            }
            std::error_code error;
            const auto bytes = fs::file_size(source, error);
            if (error) { std::snprintf(reply, size, "Scene file not found"); return sco::Result::NotFound; }
            if (bytes > 262144) { std::snprintf(reply, size, "Scene file too large"); return sco::Result::BadArg; }
            std::ifstream input(source, std::ios::binary);
            std::string text(static_cast<size_t>(bytes), '\0');
            if (!input.read(text.data(), static_cast<std::streamsize>(bytes))) {
                std::snprintf(reply, size, "Scene file unreadable"); return sco::Result::Failed;
            }
            const auto doc = hostd::Json::parse(text);
            const auto* instances = doc ? doc->find("instances") : nullptr;
            if (!instances || !instances->is_array() || instances->size() > 4096) {
                std::snprintf(reply, size, "Scene file malformed"); return sco::Result::BadArg;
            }
            struct Entry { std::string path; render::Transform transform; render::Material material; };
            std::vector<Entry> entries;
            for (size_t i = 0; i < instances->size(); ++i) {
                const auto& item = instances->at(i);
                if (!item.is_object()) { std::snprintf(reply, size, "Instance %zu not an object", i); return sco::Result::BadArg; }
                const auto* path = item.find("path");
                if (!path || !path->is_string() || path->as_string().empty() || path->as_string().size() > 4096 ||
                    vfs::NormalizePath(path->as_string()) != path->as_string() ||
                    asset::FormatOf(path->as_string()) == asset::ModelFormat::Unknown ||
                    !editor.vfs.Exists(path->as_string())) {
                    std::snprintf(reply, size, "Instance %zu has an invalid model path", i); return sco::Result::BadArg;
                }
                std::string boundWhy;
                if (!BoundsOk(item, &boundWhy)) {
                    std::snprintf(reply, size, "Instance %zu: %s", i, boundWhy.c_str()); return sco::Result::BadArg;
                }
                Entry entry;
                entry.path = path->as_string();
                const auto number = [&item](const char* key) { return static_cast<float>(item.find(key)->as_number()); };
                entry.transform.position = {number("x"), number("y"), number("z")};
                entry.transform.yaw = number("yaw");
                entry.transform.scale = number("scale");
                entry.material.baseColor = {number("red"), number("green"), number("blue")};
                entry.material.baseColor.w = number("alpha");
                entry.material.metallic = number("metallic");
                entry.material.roughness = number("roughness");
                entries.push_back(std::move(entry));
            }
            editor.scene.clear();
            editor.selected = 0;
            for (auto& entry : entries) {
                editor.Spawn(entry.path);
                auto* item = editor.Find(editor.selected);
                item->transform = entry.transform;
                item->material = entry.material;
            }
            std::snprintf(reply, size, "Loaded %zu instance(s); assets loading", entries.size()); return sco::Result::Ok;
        }, this};
    if (sco.started() && sco::RegisterCommand(this, nullptr, load) != sco::Result::Ok)
        log::Error("control", "Scene load command registration refused");
}

int Run(const Args& args) {
    log::SetSink(&LogSink);
    platform::Window window;
    platform::WindowDesc wd;
    wd.title = "scuba-engine " SCUBA_VERSION;
    wd.width = args.width; wd.height = args.height;
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
    so.productRoot = args.productRoot;
    so.dataRoot = args.data;
    ed.hostOptions = so;
    if (!args.productRoot.empty()) std::snprintf(ed.productPath, sizeof(ed.productPath), "%s", args.productRoot.string().c_str());
    if (auto r = ed.sco.Start(so); !r) log::Warn("sco", r.error);   // the editor still runs without plugins

    ed.RegisterControls();

    for (const auto& p : args.open) ed.Spawn(p);

    UiBlocker blocker(ed);
    MoveTool move(ed);
    CameraTool cameraTool(ed);
    ui::InputRouter router;
    router.Add(&blocker);
    router.Add(&move);
    router.Add(&cameraTool);

    bool quit = false;
    hostd::Registry controls;
    hostd::Broadcaster controlEvents;
    uint32_t nextFrameTicket = 0;
    std::vector<uint32_t> frameTickets;
    struct InstanceWatch { uint32_t ticket, id; };
    std::vector<InstanceWatch> instanceWatches;
    uint32_t nextInstanceTicket = 0;
    hostd::HostContext controlHost;
    std::unique_ptr<hostd::Server> controlServer;
    if (args.controlPort || !args.drive.empty()) {
        hostd::RegisterPluginHandlers(controls, controlHost);
        hostd::RegisterWidgetsHandlers(controls, controlHost);
        controls.register_handler("engine.instance.watch", [&ed, &instanceWatches, &nextInstanceTicket](const hostd::Json& params, const hostd::CallContext&) {
            const auto* id = params.find("id");
            if (!params.is_object() || params.keys().size() != 1 || !id || !id->is_number())
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Instance watch requires an existing integer id");
            const double number = id->as_number();
            if (!std::isfinite(number) || number < 1 || number > UINT32_MAX || std::floor(number) != number ||
                !ed.Find(static_cast<uint32_t>(number)))
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Instance watch requires an existing integer id");
            if (instanceWatches.size() >= 64 || nextInstanceTicket == UINT32_MAX)
                throw hostd::HandlerError(hostd::rpc_error::kUnavailable, "Instance watch capacity exhausted");
            instanceWatches.push_back({++nextInstanceTicket, static_cast<uint32_t>(number)});
            hostd::Json out = hostd::Json::MakeObject(); out.set("ticket", nextInstanceTicket); return out;
        });
        controls.register_handler("engine.vfs.get", [&ed](const hostd::Json& params, const hostd::CallContext&) {
            if (!params.is_object() || !params.keys().empty())
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "VFS read takes no arguments");
            hostd::Json mounts = hostd::Json::MakeArray();
            for (const auto& mount : ed.vfs.Mounts()) {
                hostd::Json item = hostd::Json::MakeObject();
                item.set("id", mount.id); item.set("label", mount.label); item.set("kind", mount.kind);
                item.set("mount_point", mount.mountPoint); item.set("priority", static_cast<int64_t>(mount.priority));
                item.set("order", static_cast<double>(mount.order));
                mounts.push_back(std::move(item));
            }
            hostd::Json out = hostd::Json::MakeObject(); out.set("schema", 1); out.set("mounts", std::move(mounts));
            return out;
        });
        controls.register_handler("engine.frame.request", [&frameTickets, &nextFrameTicket](const hostd::Json& params, const hostd::CallContext&) {
            if (!params.is_object() || !params.keys().empty())
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Frame request takes no arguments");
            if (frameTickets.size() >= 64 || nextFrameTicket == UINT32_MAX)
                throw hostd::HandlerError(hostd::rpc_error::kUnavailable, "Frame request capacity exhausted");
            hostd::Json out = hostd::Json::MakeObject();
            frameTickets.push_back(++nextFrameTicket);
            out.set("ticket", nextFrameTicket);
            return out;
        });
        controls.register_handler("system.methods", [&controls](const hostd::Json&, const hostd::CallContext&) {
            hostd::Json methods = hostd::Json::MakeArray();
            for (const auto& method : controls.methods()) methods.push_back(method);
            return methods;
        });
        controls.register_handler("engine.actions.list", [&controls](const hostd::Json&, const hostd::CallContext& context) {
            hostd::Json request = hostd::Json::MakeObject();
            request.set("id", 1); request.set("method", "plugins.commands");
            const auto response = controls.dispatch(request, context);
            return *response.find("result");
        });
        controls.register_handler("engine.actions.get", [&controls](const hostd::Json& params, const hostd::CallContext& context) {
            const auto* id = params.find("id");
            if (!id || !id->is_string()) throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "id must be an action name");
            hostd::Json request = hostd::Json::MakeObject();
            request.set("id", 1); request.set("method", "plugins.commands");
            const auto response = controls.dispatch(request, context);
            const auto& commands = *response.find("result");
            for (size_t i = 0; i < commands.size(); ++i)
                if (commands.at(i).find("name")->as_string() == id->as_string()) return commands.at(i);
            throw hostd::HandlerError(hostd::rpc_error::kMethodNotFound, "no such action");
        });
        controls.register_handler("engine.actions.invoke", [&controls](const hostd::Json& params, const hostd::CallContext& context) {
            if (!params.is_object()) throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "action parameters must be an object");
            for (const auto& key : params.keys())
                if (key != "id" && key != "args" && key != "request_id")
                    throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "unknown action parameter");
            const auto* id = params.find("id");
            if (!id || !id->is_string()) throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "id must be an action name");
            const auto* requestId = params.find("request_id");
            if (requestId && (!requestId->is_string() || requestId->as_string().empty() ||
                requestId->as_string().size() > 128 || requestId->as_string().find('\0') != std::string::npos))
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "request_id must be a bounded string");
            hostd::Json args = hostd::Json::MakeObject(); args.set("name", *id);
            if (const auto* values = params.find("args")) args.set("args", *values);
            hostd::Json request = hostd::Json::MakeObject();
            request.set("id", 1); request.set("method", "plugins.invoke"); request.set("params", std::move(args));
            const auto response = controls.dispatch(request, context);
            if (const auto* error = response.find("error"))
                throw hostd::HandlerError(static_cast<int>(error->find("code")->as_number()), error->find("message")->as_string());
            auto result = *response.find("result");
            if (requestId) result.set("request_id", *requestId);
            return result;
        });
        controls.register_handler("system.shutdown", [&quit](const hostd::Json&, const hostd::CallContext&) {
            quit = true;
            hostd::Json out = hostd::Json::MakeObject(); out.set("stopping", true); return out;
        });
        controls.register_handler("engine.state.get", [&ed, &window](const hostd::Json&, const hostd::CallContext&) {
            hostd::Json out = hostd::Json::MakeObject();
            out.set("schema", 1); out.set("started", ed.sco.started());
            out.set("width", window.width()); out.set("height", window.height());
            out.set("selected", ed.selected);
            hostd::Json instances = hostd::Json::MakeArray();
            for (const auto& instance : ed.scene) {
                hostd::Json item = hostd::Json::MakeObject();
                item.set("id", instance.id); item.set("name", instance.name);
                item.set("loaded", instance.model != nullptr);
                const auto load = ed.loader->Info(instance.handle);
                item.set("load_state", asset::LoadStateName(load.state));
                item.set("load_error", load.error);
                item.set("resident", instance.gpu != 0 && ed.renderer->Resident(instance.gpu));
                hostd::Json transform = hostd::Json::MakeObject();
                transform.set("x", instance.transform.position.x); transform.set("y", instance.transform.position.y);
                transform.set("z", instance.transform.position.z); transform.set("yaw", instance.transform.yaw);
                transform.set("scale", instance.transform.scale); item.set("transform", std::move(transform));
                hostd::Json material = hostd::Json::MakeObject();
                material.set("red", instance.material.baseColor.x); material.set("green", instance.material.baseColor.y);
                material.set("blue", instance.material.baseColor.z); material.set("metallic", instance.material.metallic);
                material.set("roughness", instance.material.roughness); item.set("material", std::move(material));
                instances.push_back(std::move(item));
            }
            out.set("scene", std::move(instances));
            hostd::Json camera = hostd::Json::MakeObject();
            camera.set("yaw", ed.camera.yaw); camera.set("pitch", ed.camera.pitch); camera.set("distance", ed.camera.distance);
            out.set("camera", std::move(camera));
            hostd::Json capabilities = hostd::Json::MakeArray();
            for (const auto& capability : ed.sco.MockCapabilities()) {
                hostd::Json item = hostd::Json::MakeObject();
                item.set("name", capability.name); item.set("ready", capability.ready);
                capabilities.push_back(std::move(item));
            }
            out.set("capabilities", std::move(capabilities));
            return out;
        });
        controls.register_handler("engine.screenshot", [&ed, &args](const hostd::Json& params, const hostd::CallContext&) {
            if (!params.is_object() || params.keys().size() != 1)
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Capture requires only a PNG basename");
            const auto* path = params.find("path");
            if (!path || !path->is_string() || path->as_string().empty() || path->as_string().size() > 96 ||
                !path->as_string().ends_with(".png") || args.data.empty())
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Capture requires data root and a new PNG basename");
            for (const unsigned char c : path->as_string())
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
                    throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Capture path must be a PNG basename");
            if (path->as_string().find("..") != std::string::npos)
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Capture traversal refused");
            const auto root = args.data / "captures";
            std::error_code error;
            const auto existing = fs::symlink_status(root, error);
            if (!error && fs::is_symlink(existing))
                throw hostd::HandlerError(hostd::rpc_error::kInvalidParams, "Capture directory link refused");
            fs::create_directories(root);
            const auto destination = root / path->as_string();
            std::vector<uint8_t> pixels; uint32_t width = 0, height = 0;
            auto status = ed.renderer->CaptureRgba(pixels, width, height);
            if (status) status = WriteNewRgbaPng(destination, width, height, pixels);
            if (!status) throw hostd::HandlerError(hostd::rpc_error::kHandlerFailed, status.error);
            hostd::Json out = hostd::Json::MakeObject();
            out.set("schema", 1); out.set("path", path->as_string()); out.set("width", width); out.set("height", height);
            return out;
        });
    }
    if (args.controlPort) {
        hostd::ServerOptions options; options.port = *args.controlPort;
        controlServer = std::make_unique<hostd::Server>(options, controls, controlEvents);
        std::string error;
        if (!controlServer->Start(&error)) { log::Error("control", error); quit = true; }
        else log::Info("control", "listening port=" + std::to_string(controlServer->port()));
    }
    std::optional<hostd::Json> drive;
    size_t driveIndex = 0;
    bool driveFailed = false;
    if (!args.drive.empty()) {
        std::error_code error;
        const auto size = fs::file_size(args.drive, error);
        if (!error && size <= 65536) {
            std::ifstream input(args.drive, std::ios::binary);
            std::string text(static_cast<size_t>(size), '\0');
            if (input.read(text.data(), static_cast<std::streamsize>(size))) drive = hostd::Json::parse(text);
        }
        const auto* actions = drive ? drive->find("actions") : nullptr;
        if (!actions || !actions->is_array() || actions->size() > 256) {
            log::Error("drive", "Expected bounded actions array in script"); driveFailed = true; quit = true;
        }
    }
    int frame = 0;
    while (!quit && window.Pump()) {
        if (window.TakeResized()) ed.renderer->Resize(window.width(), window.height());
        if (drive) ed.sco.Tick(static_cast<uint32_t>(driveIndex));
        else ed.sco.Tick();
        if (controlServer) controlServer->Poll(0);
        if (drive) {
            const auto& actions = *drive->find("actions");
            if (driveIndex < actions.size()) {
                auto request = actions.at(driveIndex);
                if (!request.is_object()) { driveFailed = true; quit = true; }
                else {
                    request.set("id", static_cast<uint32_t>(driveIndex + 1));
                    const auto response = controls.dispatch(request, {});
                    const auto* result = response.find("result");
                    const auto* outcome = result ? result->find("result") : nullptr;
                    if (response.find("error") || (outcome && outcome->is_string() && outcome->as_string() != "OK")) driveFailed = true;
                    log::Info("drive", response.dump());
                    if (driveFailed) quit = true;
                }
                ++driveIndex;
            } else quit = true;
        }
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
        if (!window.minimized()) {
            ed.renderer->Submit(std::move(packet));
            if (!frameTickets.empty()) {
                ed.renderer->Flush();
                const auto stats = ed.renderer->Stats();
                for (const auto ticket : frameTickets) {
                    hostd::Json ready = hostd::Json::MakeObject();
                    ready.set("ticket", ticket); ready.set("frame", static_cast<double>(stats.frames));
                    controlEvents.broadcast("engine.frame.ready", std::move(ready));
                }
                frameTickets.clear();
            }
        }
        for (auto it = instanceWatches.begin(); it != instanceWatches.end();) {
            const auto* instance = ed.Find(it->id);
            std::string outcome;
            std::string error;
            if (!instance) outcome = "removed";
            else {
                const auto load = ed.loader->Info(instance->handle);
                if (load.state == asset::LoadState::Failed || load.state == asset::LoadState::Cancelled) {
                    outcome = load.state == asset::LoadState::Failed ? "failed" : "cancelled";
                    error = load.error;
                } else if (instance->model && instance->gpu && ed.renderer->Resident(instance->gpu)) {
                    ed.renderer->Flush();
                    outcome = "ready";
                }
            }
            if (outcome.empty()) { ++it; continue; }
            hostd::Json completed = hostd::Json::MakeObject();
            completed.set("ticket", it->ticket); completed.set("id", it->id);
            completed.set("outcome", outcome); completed.set("error", error);
            controlEvents.broadcast("engine.instance.completed", std::move(completed));
            it = instanceWatches.erase(it);
        }

        if (args.frames && ++frame >= args.frames) break;
    }

    if (drive) {
        const auto* actions = drive->find("actions");
        if (!actions || !actions->is_array() || driveIndex < actions->size()) driveFailed = true;
    }
    int code = driveFailed || ((!args.drive.empty() || args.controlPort) && gErrors.load()) ? 1 : 0;
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

    if (controlServer) controlServer->Stop();
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
