// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/platform/window.h"

#include <windows.h>

namespace scuba::platform {
namespace {

constexpr wchar_t kClass[] = L"ScubaEngineWindow";

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

Window::~Window() {
    if (hwnd_) DestroyWindow(static_cast<HWND>(hwnd_));
}

Status Window::Create(const WindowDesc& desc) {
    if (hwnd_) return Status::Fail("window already created");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::Proc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);   // already registered: fine

    RECT r{0, 0, static_cast<LONG>(desc.width), static_cast<LONG>(desc.height)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kClass, Wide(desc.title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!hwnd) return Status::Fail("CreateWindowExW failed (" + std::to_string(GetLastError()) + ")");
    hwnd_ = hwnd;
    RECT client{};
    GetClientRect(hwnd, &client);
    width_ = static_cast<uint32_t>(client.right - client.left);
    height_ = static_cast<uint32_t>(client.bottom - client.top);
    ShowWindow(hwnd, desc.visible ? SW_SHOWDEFAULT : SW_HIDE);
    return Status::Ok();
}

bool Window::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closed_ = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

void Window::SetTitle(const std::string& title) {
    if (hwnd_) SetWindowTextW(static_cast<HWND>(hwnd_), Wide(title).c_str());
}

intptr_t __stdcall Window::Proc(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    const HWND h = static_cast<HWND>(hwnd);
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        static_cast<Window*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        if (self->hook_ && self->hook_(hwnd, msg, wparam, lparam)) return 1;
        return self->Handle(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(h, msg, wparam, lparam);
}

intptr_t Window::Handle(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    const HWND h = static_cast<HWND>(hwnd);
    switch (msg) {
    case WM_SIZE:
        minimized_ = wparam == SIZE_MINIMIZED;
        if (!minimized_) {
            const uint32_t w = LOWORD(lparam), hgt = HIWORD(lparam);
            if (w != width_ || hgt != height_) {
                width_ = w;
                height_ = hgt;
                resized_ = true;
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;   // Alt doesn't open the system menu
        break;
    case WM_CLOSE:
        closed_ = true;
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        closed_ = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wparam, lparam);
}

}  // namespace scuba::platform
