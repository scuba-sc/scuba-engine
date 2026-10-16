// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Input routing between the 2D shell and the 3D viewport.
//
// Layers are asked in order (first added, first asked). A layer answers with a Reply instead of
// changing global state: Handled stops the route; Capture also makes it the only receiver of
// pointer events until it Releases (or the captured button goes up). The router applies replies
// after the layer returns. Typical order: UI blocker (ImGui wants the pointer) -> gizmo / tools
// -> viewport camera and picking.
#include <cstdint>
#include <string>
#include <vector>

namespace scuba::ui {

enum class InputKind : uint8_t { PointerMove, PointerDown, PointerUp, Wheel, KeyDown };
enum class PointerButton : uint8_t { None, Left, Right, Middle };

struct InputEvent {
    InputKind kind = InputKind::PointerMove;
    float x = 0, y = 0;          // client pixels
    float dx = 0, dy = 0;        // pointer delta this frame
    PointerButton button = PointerButton::None;
    float wheel = 0;
    int key = 0;                 // ImGuiKey for KeyDown
    bool ctrl = false, shift = false, alt = false;
    bool leftHeld = false, rightHeld = false, middleHeld = false;
};

struct Reply {
    bool handled = false, capture = false, release = false;
    static Reply Unhandled() { return {}; }
    static Reply Handled() { return {true, false, false}; }
    static Reply Capture() { return {true, true, false}; }
    static Reply Release() { return {true, false, true}; }
};

class InputLayer {
public:
    virtual ~InputLayer() = default;
    virtual const char* Name() const = 0;
    virtual Reply OnInput(const InputEvent& event) = 0;
};

class InputRouter {
public:
    void Add(InputLayer* layer) { layers_.push_back(layer); }
    // Returns the layer that handled the event, or nullptr.
    InputLayer* Dispatch(const InputEvent& event);
    InputLayer* captured() const noexcept { return capture_; }

private:
    std::vector<InputLayer*> layers_;
    InputLayer* capture_ = nullptr;
    PointerButton captureButton_ = PointerButton::None;
};

}  // namespace scuba::ui
