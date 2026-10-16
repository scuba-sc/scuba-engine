// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The 2D shell: an ImGui context with docking, the Win32 platform backend and the renderer's
// ImGui backend. Each frame:
//
//   BeginFrame()   waits for the render thread to finish the previous UI frame, starts a new one
//                  and lays out a full-window dockspace whose central node is see-through: the 3D
//                  viewport shows there, and the editor's panels dock around it
//   ...            the app builds menus and panels (including sco.widgets plugin panels)
//   EndFrame()     finalizes and clones the draw data for the render thread
//
// Main thread only (it is also the sco-core game thread, which plugin panels require).
#include "scuba/core/status.h"
#include "scuba/render/camera.h"
#include "scuba/render/renderer.h"
#include "scuba/ui/input_router.h"

#include <vector>

namespace scuba::platform { class Window; }

namespace scuba::ui {

class UiLayer {
public:
    UiLayer() = default;
    ~UiLayer();
    UiLayer(const UiLayer&) = delete;
    UiLayer& operator=(const UiLayer&) = delete;

    Status Init(platform::Window& window, render::Renderer& renderer);
    void Shutdown();

    void BeginFrame();
    render::UiFramePtr EndFrame();

    // The dockspace's central node: where the 3D viewport is drawn, in client pixels.
    render::Rect viewport() const noexcept { return viewport_; }
    bool pointerOverUi() const;     // ImGui wants the pointer (over a panel, not the viewport)
    bool keyboardToUi() const;      // a text field or panel has keyboard focus
    // This frame's pointer and key input as router events, in order.
    std::vector<InputEvent> Events() const;

private:
    render::Renderer* renderer_ = nullptr;
    render::Rect viewport_;
    bool ready_ = false;
};

}  // namespace scuba::ui
