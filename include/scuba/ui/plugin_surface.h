// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "scuba/render/renderer.h"
#include <memory>
#include <string>
#include <vector>

struct ImGuiContext;
namespace scuba { class ScoHost; }
namespace scuba::ui {

// Local surface pixels, not desktop/viewport coordinates. Keys use named ImGuiKey values.
struct SurfaceInput {
    bool pointer = false, focused = false;
    float x = 0, y = 0;
    bool buttons[5]{};
    float wheelX = 0, wheelY = 0;
    struct Key { int key; bool down; };
    std::vector<Key> keys;
    std::string text;
};

// Main/game thread only. The main context/font atlas and its texture resources must outlive
// this surface AND every returned frame. Obey Renderer::BeginUiFrame's texture-update barrier.
class PluginSurface {
public:
    PluginSurface();
    ~PluginSurface();
    PluginSurface(const PluginSurface&) = delete;
    PluginSurface& operator=(const PluginSurface&) = delete;
    Status Init(ImGuiContext* mainContext);
    Status Render(ScoHost& host, const std::string& panelId, uint32_t width, uint32_t height,
                  float deltaSeconds, const SurfaceInput& input, render::UiFramePtr& output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PanelHit { float u = 0, v = 0, distance = 0; };
// Renderer unit XY quad [-.5,.5], front +Z; UV origin top-left. Affine row-vector world.
bool HitPanel(const render::Ray& ray, const render::Float4x4& world, PanelHit& hit);
bool HitPanel(const render::OrbitCamera& camera, float x, float y, const render::Rect& viewport,
              const render::Float4x4& world, PanelHit& hit);
} // namespace scuba::ui
