// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/input_router.h"

namespace scuba::ui {

InputLayer* InputRouter::Dispatch(const InputEvent& event) {
    if (event.kind == InputKind::PointerCancel) {
        if (!capture_) return nullptr;
        InputLayer* holder = capture_;
        suppressedReleaseButton_ = captureButton_;
        holder->OnInput(event);
        capture_ = nullptr;
        captureButton_ = PointerButton::None;
        return holder;
    }
    if (suppressedReleaseButton_ != PointerButton::None && event.kind == InputKind::PointerDown &&
        event.button == suppressedReleaseButton_)
        suppressedReleaseButton_ = PointerButton::None;
    if (suppressedReleaseButton_ != PointerButton::None && event.kind == InputKind::PointerUp &&
        event.button == suppressedReleaseButton_) {
        suppressedReleaseButton_ = PointerButton::None;
        return nullptr;
    }
    const bool pointer = event.kind != InputKind::KeyDown;
    if (capture_ && pointer) {
        const Reply r = capture_->OnInput(event);
        InputLayer* holder = capture_;
        const bool buttonUp = event.kind == InputKind::PointerUp && event.button == captureButton_;
        if (r.release || buttonUp) {
            if (r.release && !buttonUp) suppressedReleaseButton_ = captureButton_;
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
        } else if (r.release && capture_) {
            suppressedReleaseButton_ = captureButton_;
            capture_ = nullptr;
            captureButton_ = PointerButton::None;
        }
        return layer;
    }
    return nullptr;
}

}  // namespace scuba::ui
