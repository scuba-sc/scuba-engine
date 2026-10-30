// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The renderer front end. A backend (D3D12 today; Vulkan next) owns the GPU and a render thread.
//
//   main thread                               render thread
//   ───────────                               ─────────────
//   Upload(model)  ─── upload queue ──────►   copy into GPU buffers at the start of a frame
//   BeginUiFrame() waits for UI frame N-1 ◄── signals "UI consumed" once it recorded frame N-1's UI
//   build UI + scene, Submit(packet N) ─────► record scene, grid, UI; present; fence
//
// The main thread runs at most one frame ahead: Submit blocks while a packet is still waiting.
// GPU objects die through a fence-gated retire list, never while a frame in flight uses them.
// Nothing here exposes D3D12 or ImGui types.
#include "scuba/asset/model.h"
#include "scuba/core/status.h"
#include "scuba/render/camera.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scuba::render {

using GpuModel = uint32_t;   // 0 is never valid
using GpuPanel = uint32_t;   // 0 is never valid; a render-to-texture surface the scene samples

// Metallic-roughness PBR without textures (phase 1): constant factors per draw.
struct Material {
    Float4 baseColor{0.78f, 0.79f, 0.82f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.55f;
};

struct DrawItem {
    GpuModel model = 0;
    Float4x4 world;
    Material material;
    bool selected = false;
};

struct FrameView {
    Float4x4 view, projection;
    Float3 eye;
    Float3 sunDirection{-0.45f, -0.35f, -0.82f};   // the direction light travels
    Rect viewport;                                   // 3D area in client pixels
    Float4 clear{0.075f, 0.082f, 0.098f, 1.0f};
    bool grid = true;
};

struct UiFrame;   // an ImGui frame's cloned draw lists (made by UiLayer)
struct UiFrameDeleter {
    void operator()(UiFrame* frame) const;
};
using UiFramePtr = std::unique_ptr<UiFrame, UiFrameDeleter>;

// A diegetic panel: an off-screen surface filled with its own cloned plugin UI, then sampled by
// one world-space quad. The editor's draw list is never used as surface content.
struct PanelDraw {
    GpuPanel panel = 0;
    Float4x4 world;      // the quad's transform: unit quad in XY, facing +Z before the transform
    bool content = true; // false leaves the surface as it is (the panel was filled earlier)
    UiFramePtr ui;       // this surface only; never falls back to the editor draw list
};

struct FramePacket {
    FrameView view;
    std::vector<DrawItem> items;
    UiFramePtr ui;
    std::vector<PanelDraw> panels;
};

struct RendererDesc {
    void* window = nullptr;   // HWND
    uint32_t width = 0, height = 0;
    bool vsync = true;
    bool debugLayer = false;
    bool warp = false;        // software adapter (CI, no GPU)
    // Render into off-screen targets and never present: the smoke path (--frames) has to run
    // without a desktop, and a hidden window cannot host a swap chain.
    bool offscreen = false;
};

struct RendererStats {
    std::string backend, adapter;
    uint64_t frames = 0;
    uint32_t drawCalls = 0, instances = 0, residentModels = 0, pendingUploads = 0;
    double cpuRecordMs = 0;
};

class Renderer {
public:
    static std::unique_ptr<Renderer> CreateD3D12(const RendererDesc& desc, Status& status);
    virtual ~Renderer() = default;

    // Main thread. The model is copied to the GPU by the render thread; draws that name it are
    // skipped until it is resident.
    virtual GpuModel Upload(std::shared_ptr<const asset::ModelData> model) = 0;
    virtual void Release(GpuModel model) = 0;
    virtual bool Resident(GpuModel model) const = 0;

    // ImGui backend (the ImGui context must exist). Main thread.
    virtual Status InitUi() = 0;
    virtual void ShutdownUi() = 0;
    // Waits until the render thread has recorded the previous UI frame, then starts the backend's
    // new frame. Call before ImGui::NewFrame.
    virtual void BeginUiFrame() = 0;

    // Diegetic panels: a surface with its own colour target and shader-visible view, filled by the
    // frame's UI draw list when a PanelDraw asks for content, then sampled by a world-space quad.
    // Main thread; the surface lives in the renderer's own heaps and dies with ReleasePanel.
    virtual GpuPanel CreatePanel(uint32_t width, uint32_t height) = 0;
    virtual void ReleasePanel(GpuPanel panel) = 0;
    // Copies the last submitted frame's colour target back as RGBA8. Only an off-screen renderer can
    // answer this: a swap-chain frame has no stable surface to read.
    virtual Status Readback(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) = 0;

    virtual void Submit(FramePacket packet) = 0;
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    // Waits for the render thread and the GPU to go idle.
    virtual void Flush() = 0;
    virtual RendererStats Stats() const = 0;
};

}  // namespace scuba::render
