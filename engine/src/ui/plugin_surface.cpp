// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/plugin_surface.h"
#include "scuba/sco/sco_host.h"
#include "ui/ui_frame.h"
#include "sco/widgets_imgui.h"
#include "imgui.h"
#include <cmath>
#include <cfloat>

namespace scuba::ui {
namespace {
struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
}
struct PluginSurface::Impl {
    ImGuiContext* context = nullptr;
    ImVector<ImTextureData*>* textures = nullptr;
};
PluginSurface::PluginSurface() : impl_(std::make_unique<Impl>()) {}
PluginSurface::~PluginSurface() {
    if (impl_->context) {
        ContextScope scope(impl_->context);
        ImGui::DestroyContext(impl_->context);
    }
}
Status PluginSurface::Init(ImGuiContext* mainContext) {
    if (!mainContext || impl_->context) return Status::Fail("invalid or initialized plugin surface");
    ContextScope scope(mainContext);
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const int rendererFlags = ImGui::GetIO().BackendFlags & ImGuiBackendFlags_RendererHasTextures;
    impl_->textures = &ImGui::GetPlatformIO().Textures;
    impl_->context = ImGui::CreateContext(atlas);
    // CreateContext restores the caller when one is current; configure the private IO.
    ImGui::SetCurrentContext(impl_->context);
    ImGui::GetIO().BackendFlags |= rendererFlags;
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    return Status::Ok();
}
Status PluginSurface::Render(ScoHost& host, const std::string& panelId, uint32_t width,
                             uint32_t height, float deltaSeconds, const SurfaceInput& input,
                             render::UiFramePtr& output) {
    output.reset();
    if (!impl_->context || !host.started()) return Status::Fail("plugin surface or host not started");
    if (!width || !height || !std::isfinite(deltaSeconds) || deltaSeconds <= 0)
        return Status::Fail("invalid plugin surface dimensions or delta");
    ContextScope scope(impl_->context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
    io.DeltaTime = deltaSeconds;
    io.AddFocusEvent(input.focused);
    const bool pointer = input.pointer && std::isfinite(input.x) && std::isfinite(input.y);
    io.AddMousePosEvent(pointer ? input.x : -FLT_MAX, pointer ? input.y : -FLT_MAX);
    for (int b = 0; b < 5; ++b) io.AddMouseButtonEvent(b, input.focused && input.buttons[b]);
    if (input.focused) {
        if (std::isfinite(input.wheelX) && std::isfinite(input.wheelY))
            io.AddMouseWheelEvent(input.wheelX, input.wheelY);
        for (const auto& key : input.keys)
            if ((key.key >= ImGuiKey_NamedKey_BEGIN && key.key < ImGuiKey_NamedKey_END) ||
                key.key == ImGuiMod_Ctrl || key.key == ImGuiMod_Shift ||
                key.key == ImGuiMod_Alt || key.key == ImGuiMod_Super)
                io.AddKeyEvent(static_cast<ImGuiKey>(key.key), key.down);
        io.AddInputCharactersUTF8(input.text.c_str());
    }
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("###plugin-surface", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    sco::widgets::ImGuiRenderer renderer;
    Status status = host.RenderPanel(panelId, renderer);
    ImGui::End();
    ImGui::Render();
    if (!status) return status;
    const ImDrawData* source = ImGui::GetDrawData();
    render::UiFramePtr frame(new render::UiFrame());
    frame->data = *source;
    frame->data.CmdLists.resize(0);
    frame->lists.reserve(static_cast<size_t>(source->CmdListsCount));
    frame->data.CmdLists.reserve(source->CmdListsCount);
    frame->data.OwnerViewport = nullptr;
    // The shared font atlas belongs to the main context. Never retain the private context's
    // PlatformIO texture vector, which dies with this surface.
    frame->data.Textures = impl_->textures;
    for (ImDrawList* list : source->CmdLists) {
        ImDrawList* copy = list->CloneOutput();
        frame->lists.push_back(copy);
        frame->data.CmdLists.push_back(copy);
    }
    output = std::move(frame);
    return Status::Ok();
}
namespace {
using render::Float3;
Float3 Sub(Float3 a, Float3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Float3 Cross(Float3 a, Float3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
float Dot(Float3 a, Float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
bool Finite(Float3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
}
bool HitPanel(const render::Ray& ray, const render::Float4x4& world, PanelHit& hit) {
    for (const auto& row : world.m)
        for (float value : row) if (!std::isfinite(value)) return false;
    if (!Finite(ray.origin) || !Finite(ray.dir) || world.m[0][3] != 0 || world.m[1][3] != 0 ||
        world.m[2][3] != 0 || world.m[3][3] != 1) return false;
    const Float3 x{world.m[0][0], world.m[0][1], world.m[0][2]};
    const Float3 y{world.m[1][0], world.m[1][1], world.m[1][2]};
    const Float3 origin{world.m[3][0], world.m[3][1], world.m[3][2]};
    const Float3 normal = Cross(x, y);
    const float n2 = Dot(normal, normal), d2 = Dot(ray.dir, ray.dir);
    const float facing = Dot(ray.dir, normal);
    if (!std::isfinite(n2) || !std::isfinite(d2) || n2 <= 1e-20f || d2 <= 1e-20f ||
        facing >= -1e-6f * std::sqrt(n2 * d2)) return false;
    const float t = Dot(Sub(origin, ray.origin), normal) / facing;
    if (!std::isfinite(t) || t < 0) return false;
    const Float3 p = Sub({ray.origin.x + t*ray.dir.x, ray.origin.y + t*ray.dir.y,
                         ray.origin.z + t*ray.dir.z}, origin);
    // Dual basis handles nonuniform scale and shear, unlike projecting onto normalized axes.
    const float localX = Dot(Cross(p, y), normal) / n2;
    const float localY = Dot(Cross(x, p), normal) / n2;
    if (!std::isfinite(localX) || !std::isfinite(localY) || localX < -.5f || localX > .5f ||
        localY < -.5f || localY > .5f) return false;
    hit = {localX + .5f, .5f - localY, t * std::sqrt(d2)};
    return std::isfinite(hit.distance);
}
bool HitPanel(const render::OrbitCamera& camera, float x, float y, const render::Rect& viewport,
              const render::Float4x4& world, PanelHit& hit) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(viewport.x) ||
        !std::isfinite(viewport.y) || !std::isfinite(viewport.w) || !std::isfinite(viewport.h) ||
        viewport.w <= 0 || viewport.h <= 0 || !viewport.Contains(x, y)) return false;
    return HitPanel(camera.ScreenRay(x, y, viewport), world, hit);
}
} // namespace scuba::ui
