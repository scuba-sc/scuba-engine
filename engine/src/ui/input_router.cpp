// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/input_router.h"

namespace scuba::ui {

InputLayer* InputRouter::Dispatch(const InputEvent& event) {
    const bool pointer = event.kind != InputKind::KeyDown;
    if (capture_ && pointer) {
        const Reply r = capture_->OnInput(event);
        InputLayer* holder = capture_;
        const bool buttonUp = event.kind == InputKind::PointerUp && event.button == captureButton_;
        if (r.release || buttonUp) {
            capture_ = nullptr;
            captureButton_ = PointerButton::None;
        }
        return holder;
    }
    for (InputLayer* layer : layers_) {
        const Reply r = layer->OnInput(event);
        if (!r.handled) continue;
        if (r.capture && pointer) {
            capture_ = layer;
            captureButton_ = event.button;
        }
        return layer;
    }
    return nullptr;
}

}  // namespace scuba::ui
