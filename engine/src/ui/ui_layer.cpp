// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/ui_layer.h"
#include "scuba/platform/window.h"
#include "ui/ui_frame.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"

#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace scuba::ui {
namespace {

intptr_t ImGuiHook(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    return ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, wparam, lparam);
}

void Style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 3;
    s.TabRounding = 3;
    s.ScrollbarRounding = 3;
    s.WindowBorderSize = 1;
    s.WindowPadding = {12, 12};
    s.WindowMinSize = {220, 120};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {8, 8};
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = {0.89f, 0.91f, 0.93f, 1};
    c[ImGuiCol_TextDisabled] = {0.57f, 0.63f, 0.68f, 1};
    c[ImGuiCol_WindowBg] = {0.105f, 0.118f, 0.133f, 1};
    c[ImGuiCol_Border] = {0.22f, 0.25f, 0.28f, 1};
    c[ImGuiCol_FrameBg] = {0.145f, 0.165f, 0.184f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.19f, 0.25f, 0.28f, 1};
    c[ImGuiCol_FrameBgActive] = {0.19f, 0.37f, 0.38f, 1};
    c[ImGuiCol_ChildBg] = {0.095f, 0.100f, 0.115f, 1.0f};
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = {0.080f, 0.086f, 0.100f, 1.0f};
    c[ImGuiCol_MenuBarBg] = {0.080f, 0.086f, 0.100f, 1.0f};
    c[ImGuiCol_Header] = {0.19f, 0.47f, 0.47f, 0.55f};
    c[ImGuiCol_HeaderHovered] = {0.22f, 0.40f, 0.55f, 0.85f};
    c[ImGuiCol_Button] = {0.17f, 0.24f, 0.31f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.22f, 0.36f, 0.48f, 1.0f};
    c[ImGuiCol_Tab] = {0.12f, 0.14f, 0.17f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.16f, 0.30f, 0.31f, 1};
    c[ImGuiCol_DockingEmptyBg] = {0, 0, 0, 0};
}

}  // namespace

UiLayer::~UiLayer() { Shutdown(); }

Status UiLayer::Init(platform::Window& window, render::Renderer& renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "scuba-engine-workspace-v2.ini";
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingAlwaysTabBar = true;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 15.0f);
    Style();
    if (!ImGui_ImplWin32_Init(window.native())) return Status::Fail("ImGui_ImplWin32_Init failed");
    window.SetMessageHook(&ImGuiHook);
    if (auto st = renderer.InitUi(); !st) return st;
    renderer_ = &renderer;
    ready_ = true;
    return Status::Ok();
}

void UiLayer::Shutdown() {
    if (!ready_) return;
    renderer_->ShutdownUi();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ready_ = false;
}

void UiLayer::BeginFrame() {
    renderer_->BeginUiFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12, 3});
    if (ImGui::BeginViewportSideBar("##workspace-status", ImGui::GetMainViewport(), ImGuiDir_Down,
                                   ImGui::GetTextLineHeight() + 6, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        const auto stats = renderer_->Stats();
        ImGui::TextDisabled("MOCK SANDBOX   |   %s   |   %.0f fps   |   %u models resident",
                            stats.backend.c_str(), ImGui::GetIO().Framerate, stats.residentModels);
        ImGui::SameLine();
        ImGui::TextDisabled("   RMB orbit   MMB pan   Wheel zoom   F frame");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const ImGuiID dock = ImGui::GetID("ScubaWorkspace");
    if (resetWorkspace_ || !ImGui::DockBuilderGetNode(dock)) {
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) | ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockBuilderSetNodePos(dock, main->WorkPos);
        ImGui::DockBuilderSetNodeSize(dock, main->WorkSize);
        ImGuiID center = dock, left = 0, right = 0, bottom = 0;
        const float leftWidth = main->WorkSize.x > 1400 ? 250.0f : 220.0f;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, leftWidth / main->WorkSize.x, &left, &center);
        const float rightWidth = 340.0f;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, rightWidth / (main->WorkSize.x - leftWidth), &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.23f, &bottom, &center);
        for (const char* name : {"Scene", "Assets", "Designer"}) ImGui::DockBuilderDockWindow(name, left);
        for (const char* name : {"Inspector", "Plugins", "Plugin controls"}) ImGui::DockBuilderDockWindow(name, right);
        for (const char* name : {"Console", "Mounts", "Asset loader", "Stats"}) ImGui::DockBuilderDockWindow(name, bottom);
        ImGui::DockBuilderFinish(dock);
        resetWorkspace_ = false;
    }
    ImGui::DockSpaceOverViewport(dock, main, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode);
    if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dock)) {
        const ImVec2 origin = ImGui::GetMainViewport()->Pos;
        viewport_ = {central->Pos.x - origin.x, central->Pos.y - origin.y, central->Size.x, central->Size.y};
    } else {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        viewport_ = {vp->WorkPos.x - vp->Pos.x, vp->WorkPos.y - vp->Pos.y, vp->WorkSize.x, vp->WorkSize.y};
    }
}

render::UiFramePtr UiLayer::EndFrame() {
    ImGui::Render();
    const ImDrawData* src = ImGui::GetDrawData();
    render::UiFramePtr frame(new render::UiFrame());
    frame->data = *src;
    frame->data.CmdLists.resize(0);
    for (ImDrawList* list : src->CmdLists) {
        ImDrawList* copy = list->CloneOutput();
        frame->lists.push_back(copy);
        frame->data.CmdLists.push_back(copy);
    }
    return frame;
}

bool UiLayer::pointerOverUi() const { return ImGui::GetIO().WantCaptureMouse; }
bool UiLayer::keyboardToUi() const { return ImGui::GetIO().WantCaptureKeyboard; }

std::vector<InputEvent> UiLayer::Events() const {
    const ImGuiIO& io = ImGui::GetIO();
    std::vector<InputEvent> out;
    InputEvent base;
    base.x = io.MousePos.x;
    base.y = io.MousePos.y;
    base.dx = io.MouseDelta.x;
    base.dy = io.MouseDelta.y;
    base.ctrl = io.KeyCtrl;
    base.shift = io.KeyShift;
    base.alt = io.KeyAlt;
    base.leftHeld = io.MouseDown[0];
    base.rightHeld = io.MouseDown[1];
    base.middleHeld = io.MouseDown[2];
    if (!ImGui::IsMousePosValid(&io.MousePos)) return out;

    static constexpr PointerButton kButtons[3] = {PointerButton::Left, PointerButton::Right, PointerButton::Middle};
    for (int b = 0; b < 3; ++b)
        if (io.MouseClicked[b]) {
            InputEvent e = base;
            e.kind = InputKind::PointerDown;
            e.button = kButtons[b];
            out.push_back(e);
        }
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) {
        InputEvent e = base;
        e.kind = InputKind::PointerMove;
        out.push_back(e);
    }
    if (io.MouseWheel != 0) {
        InputEvent e = base;
        e.kind = InputKind::Wheel;
        e.wheel = io.MouseWheel;
        out.push_back(e);
    }
    for (int b = 0; b < 3; ++b)
        if (io.MouseReleased[b]) {
            InputEvent e = base;
            e.kind = InputKind::PointerUp;
            e.button = kButtons[b];
            out.push_back(e);
        }
    for (ImGuiKey key : {ImGuiKey_F, ImGuiKey_Delete, ImGuiKey_Escape, ImGuiKey_G})
        if (ImGui::IsKeyPressed(key, false)) {
            InputEvent e = base;
            e.kind = InputKind::KeyDown;
            e.key = key;
            out.push_back(e);
        }
    return out;
}

}  // namespace scuba::ui
