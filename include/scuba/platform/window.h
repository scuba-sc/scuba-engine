// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// The OS window. Created and pumped on the main thread (which is also sco-core's game thread).
// Windows only in phase 1; the interface carries no OS types.
#include "scuba/core/status.h"

#include <cstdint>
#include <string>

namespace scuba::platform {

struct WindowDesc {
    std::string title = "scuba-engine";   // UTF-8
    uint32_t width = 1600, height = 900;
    bool visible = true;
};

// Sees every OS message before the window does; nonzero means consumed. ImGui's Win32 backend
// handler is installed here.
using MessageHook = intptr_t (*)(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Status Create(const WindowDesc& desc);
    // Drains the message queue without blocking. False once the window has been closed.
    bool Pump();
    void SetMessageHook(MessageHook hook) noexcept { hook_ = hook; }
    void SetTitle(const std::string& title);

    void* native() const noexcept { return hwnd_; }
    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    bool minimized() const noexcept { return minimized_; }
    // True once after the client size changed.
    bool TakeResized() noexcept {
        const bool r = resized_;
        resized_ = false;
        return r;
    }

private:
    static intptr_t __stdcall Proc(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);
    intptr_t Handle(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);

    void* hwnd_ = nullptr;
    MessageHook hook_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    bool minimized_ = false, resized_ = false, closed_ = false;
};

}  // namespace scuba::platform
