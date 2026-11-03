// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/input_router.h"

#include <iostream>
#include <vector>

namespace {

int failures = 0;
constexpr int kReleaseKey = 1;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; ++failures; \
} } while (false)

class CaptureLayer final : public scuba::ui::InputLayer {
public:
    const char* Name() const override { return "capture"; }
    scuba::ui::Reply OnInput(const scuba::ui::InputEvent& event) override {
        events.push_back(event.kind);
        if (event.kind == scuba::ui::InputKind::PointerDown)
            return scuba::ui::Reply::Capture();
        if (event.kind == scuba::ui::InputKind::PointerCancel)
            return scuba::ui::Reply::Handled();
        if (event.kind == scuba::ui::InputKind::PointerMove && releaseOnMove)
            return scuba::ui::Reply::Release();
        if (event.kind == scuba::ui::InputKind::KeyDown && event.key == kReleaseKey)
            return scuba::ui::Reply::Release();
        return scuba::ui::Reply::Unhandled();
    }

    bool releaseOnMove = false;
    std::vector<scuba::ui::InputKind> events;
};

class FallbackLayer final : public scuba::ui::InputLayer {
public:
    const char* Name() const override { return "fallback"; }
    scuba::ui::Reply OnInput(const scuba::ui::InputEvent& event) override {
        events.push_back(event.kind);
        return scuba::ui::Reply::Handled();
    }

    std::vector<scuba::ui::InputKind> events;
};

class HandleLayer final : public scuba::ui::InputLayer {
public:
    const char* Name() const override { return "gizmo-handle"; }
    scuba::ui::Reply OnInput(const scuba::ui::InputEvent& event) override {
        events.push_back(event.kind);
        if (event.kind == scuba::ui::InputKind::PointerDown && event.x == 10)
            return scuba::ui::Reply::Capture();
        return scuba::ui::Reply::Unhandled();
    }

    std::vector<scuba::ui::InputKind> events;
};

class PlacementLayer final : public scuba::ui::InputLayer {
public:
    const char* Name() const override { return "placement-preview"; }
    scuba::ui::Reply OnInput(const scuba::ui::InputEvent& event) override {
        events.push_back(event.kind);
        if (active && event.kind == scuba::ui::InputKind::PointerDown)
            return scuba::ui::Reply::Capture();
        return scuba::ui::Reply::Unhandled();
    }

    bool active = true;
    std::vector<scuba::ui::InputKind> events;
};

class PickLayer final : public scuba::ui::InputLayer {
public:
    const char* Name() const override { return "model-pick"; }
    scuba::ui::Reply OnInput(const scuba::ui::InputEvent& event) override {
        events.push_back(event.kind);
        if (event.kind == scuba::ui::InputKind::PointerDown)
            return scuba::ui::Reply::Capture();
        if (event.kind == scuba::ui::InputKind::PointerUp) {
            ++picks;
            return scuba::ui::Reply::Release();
        }
        return scuba::ui::Reply::Unhandled();
    }

    int picks = 0;
    std::vector<scuba::ui::InputKind> events;
};

scuba::ui::InputEvent Event(scuba::ui::InputKind kind, float x = 0) {
    scuba::ui::InputEvent event;
    event.kind = kind;
    event.x = x;
    event.button = scuba::ui::PointerButton::Left;
    return event;
}

}  // namespace

int main() {
    using namespace scuba::ui;
    InputRouter router;
    CaptureLayer capture;
    FallbackLayer fallback;
    router.Add(&capture);
    router.Add(&fallback);

    CHECK(router.Dispatch(Event(InputKind::PointerDown)) == &capture);
    CHECK(router.captured() == &capture);
    CHECK(router.Dispatch(Event(InputKind::PointerMove)) == &capture);
    CHECK(router.Dispatch(Event(InputKind::PointerCancel)) == &capture);
    CHECK(router.captured() == nullptr);
    CHECK(capture.events.size() == 3);
    CHECK(capture.events.back() == InputKind::PointerCancel);
    CHECK(fallback.events.empty());
    CHECK(router.Dispatch(Event(InputKind::PointerUp)) == nullptr);  // canceled release cannot become a click
    CHECK(capture.events.size() == 3 && fallback.events.empty());
    CHECK(router.Dispatch(Event(InputKind::PointerDown)) == &capture);  // a new gesture clears a missing-release guard
    CHECK(router.captured() == &capture);
    CHECK(router.Dispatch(Event(InputKind::PointerUp)) == &capture);
    CHECK(router.captured() == nullptr);

    CHECK(router.Dispatch(Event(InputKind::PointerDown)) == &capture);
    InputEvent releaseKey = Event(InputKind::KeyDown);
    releaseKey.key = kReleaseKey;
    CHECK(router.Dispatch(releaseKey) == &capture);
    CHECK(router.captured() == nullptr);
    CHECK(router.Dispatch(Event(InputKind::PointerUp)) == nullptr);  // Escape consumes the eventual button-up
    CHECK(fallback.events.empty());

    CHECK(router.Dispatch(Event(InputKind::PointerDown)) == &capture);
    CHECK(router.Dispatch(Event(InputKind::PointerUp)) == &capture);
    CHECK(router.captured() == nullptr);

    capture.releaseOnMove = true;
    CHECK(router.Dispatch(Event(InputKind::PointerDown)) == &capture);
    CHECK(router.Dispatch(Event(InputKind::PointerMove)) == &capture);
    CHECK(router.captured() == nullptr);
    CHECK(router.Dispatch(Event(InputKind::PointerUp)) == nullptr);
    CHECK(fallback.events.empty());

    // Placement preview owns viewport clicks first. Once inactive, a gizmo hit captures through
    // pointer-up and cannot leak into the lower-priority model picker.
    InputRouter priorityRouter;
    PlacementLayer placement;
    HandleLayer gizmo;
    PickLayer picker;
    priorityRouter.Add(&placement);
    priorityRouter.Add(&gizmo);
    priorityRouter.Add(&picker);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerDown, 10)) == &placement);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerMove, 200)) == &placement);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerUp, 200)) == &placement);
    CHECK(gizmo.events.empty() && picker.picks == 0);
    placement.active = false;
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerDown, 10)) == &gizmo);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerMove, 200)) == &gizmo);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerUp, 200)) == &gizmo);
    CHECK(picker.picks == 0);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerDown, 200)) == &picker);
    CHECK(priorityRouter.Dispatch(Event(InputKind::PointerUp, 200)) == &picker);
    CHECK(picker.picks == 1);

    CHECK(failures == 0);
    return failures == 0 ? 0 : 1;
}
