// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/ui/plugin_surface.h"
#include "scuba/sco/sco_host.h"
#include "ui/ui_frame.h"
#include "imgui.h"
#include "sco/widgets.h"
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <cmath>

// Portable tests don't link the D3D backend which normally supplies this.
namespace scuba::render {
void UiFrameDeleter::operator()(UiFrame* frame) const { delete frame; }
}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

int main(int argc, char** argv) {
    using namespace scuba;
    ui::PanelHit hit;
    render::Float4x4 world;
    CHECK(ui::HitPanel(render::Ray{{0.25f, 0.25f, 2}, {0, 0, -1}}, world, hit));
    CHECK(std::abs(hit.u - .75f) < 1e-5f && std::abs(hit.v - .25f) < 1e-5f);
    CHECK(std::abs(hit.distance - 2) < 1e-5f);
    CHECK(!ui::HitPanel(render::Ray{{0, 0, -2}, {0, 0, 1}}, world, hit));
    CHECK(!ui::HitPanel(render::Ray{{0, 0, 2}, {1, 0, 0}}, world, hit));
    CHECK(!ui::HitPanel(render::Ray{{1, 0, 2}, {0, 0, -1}}, world, hit));
    CHECK(!ui::HitPanel(render::Ray{{0, 0, 2}, {0, 0, 1}}, world, hit));
    world.m[0][0] = 2; world.m[1][0] = .5f; world.m[1][1] = 3;
    world.m[3][0] = 4; world.m[3][1] = 5; world.m[3][2] = 6;
    CHECK(ui::HitPanel(render::Ray{{4.625f, 5.75f, 8}, {0, 0, -1}}, world, hit));
    CHECK(std::abs(hit.u - .75f) < 1e-5f && std::abs(hit.v - .25f) < 1e-5f);
    world.m[0][0] = 0;
    CHECK(!ui::HitPanel(render::Ray{{4, 5, 8}, {0, 0, -1}}, world, hit));
    world = {};
    world.m[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!ui::HitPanel(render::Ray{{0, 0, 2}, {0, 0, -1}}, world, hit));
    world = {};
    render::OrbitCamera camera;
    CHECK(ui::HitPanel(camera, 400, 300, {0, 0, 800, 600}, world, hit));
    CHECK(std::abs(hit.u - .5f) < 1e-4f && std::abs(hit.v - .5f) < 1e-4f);
    CHECK(!ui::HitPanel(camera, -1, 300, {0, 0, 800, 600}, world, hit));
    CHECK(!ui::HitPanel(camera, 0, 0, {0, 0, 0, 0}, world, hit));
    camera.fovY = std::numeric_limits<float>::quiet_NaN();
    camera.pitch = -1;
    CHECK(!ui::HitPanel(camera, 400, 300, {0, 0, 800, 600}, world, hit));
    ImGuiContext* main = ImGui::CreateContext();
    unsigned char* pixels; int w, h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    ImGui::GetIO().Fonts->SetTexID(1);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = {800, 600};
    ImGui::NewFrame();
    ImGui::Render();
    {
        scuba::ui::PluginSurface surface;
        // CreateContext restores the caller; initialization must select the private IO.
        const char* sentinelIni = "editor-layout-must-survive.ini";
        ImGui::GetIO().IniFilename = sentinelIni;
        CHECK(surface.Init(main));
        CHECK(ImGui::GetIO().IniFilename == sentinelIni);
        ImGui::GetIO().IniFilename = nullptr;
        CHECK(ImGui::GetCurrentContext() == main);
        scuba::ScoHost host;
        scuba::render::UiFramePtr output;
        CHECK(!surface.Render(host, "missing", 320, 240, 1.0f / 60, {}, output));
        CHECK(!output);
        CHECK(ImGui::GetCurrentContext() == main);
        scuba::ScoHostOptions options;
        options.pluginRoot = argc > 1 ? argv[1] : "build/plugins/runtime";
        options.lua = false;
        CHECK(host.Start(options));
        host.Tick();
        CHECK(!host.Panels().empty());
        ImGui::NewFrame();
        ImGui::Begin("Editor-only sentinel");
        ImGui::TextUnformatted("Not plugin UI");
        ImGui::End();
        const int editorVertices = ImGui::GetBackgroundDrawList()->VtxBuffer.Size;
        CHECK(surface.Render(host, "mp_chat.main", 640, 480, 1.0f / 60, {}, output));
        CHECK(ImGui::GetBackgroundDrawList()->VtxBuffer.Size == editorVertices);
        ImGui::Render();
        CHECK(output && output->data.TotalVtxCount > 0);
        CHECK(output->data.DisplaySize.x == 640);
        CHECK(output->data.Textures == &ImGui::GetPlatformIO().Textures);
        CHECK(output->data.OwnerViewport == nullptr);
        const auto firstVertex = output->lists[0]->VtxBuffer[0].pos;
        auto retained = std::move(output);
        CHECK(surface.Render(host, "mp_chat.main", 320, 240, 1.0f / 60, {}, output));
        CHECK(retained->data.DisplaySize.x == 640);
        CHECK(retained->lists[0] != output->lists[0]);
        CHECK(retained->lists[0]->VtxBuffer[0].pos.x == firstVertex.x);
        CHECK(!surface.Render(host, "missing", 640, 480, 1.0f / 60, {}, output));
        CHECK(!output);
        bool edited = false;
        for (int y = 20; y < 470 && !edited; y += 20) {
            scuba::ui::SurfaceInput input;
            input.pointer = input.focused = true;
            input.x = 80; input.y = static_cast<float>(y);
            input.buttons[0] = true;
            CHECK(surface.Render(host, "mp_chat.main", 640, 480, 1.0f / 60, input, output));
            input.buttons[0] = false;
            CHECK(surface.Render(host, "mp_chat.main", 640, 480, 1.0f / 60, input, output));
            input.text = "surface-probe";
            CHECK(surface.Render(host, "mp_chat.main", 640, 480, 1.0f / 60, input, output));
            host.Tick();
            for (const auto& panel : sco::widgets::Panels())
                if (panel.id == "mp_chat.main")
                    for (const auto& node : panel.nodes)
                        if (node.kind == SCO_WIDGET_TEXT_INPUT && node.text.find("surface-probe") != std::string::npos)
                            edited = true;
        }
        CHECK(edited);
        scuba::ui::SurfaceInput backspace;
        backspace.focused = true;
        backspace.keys.push_back({ImGuiKey_Backspace, true});
        CHECK(surface.Render(host, "mp_chat.main", 640, 480, 1.0f / 60, backspace, output));
        host.Tick();
        bool erased = false;
        for (const auto& panel : sco::widgets::Panels())
            if (panel.id == "mp_chat.main")
                for (const auto& node : panel.nodes)
                    if (node.kind == SCO_WIDGET_TEXT_INPUT && node.text == "surface-prob") erased = true;
        CHECK(erased);
        CHECK(ImGui::GetIO().InputQueueCharacters.empty());
        CHECK(!ImGui::GetIO().MouseDown[0]);
        scuba::ui::SurfaceInput invalid;
        CHECK(!surface.Render(host, "mp_chat.main", 0, 480, 1.0f / 60, invalid, output));
        CHECK(!output);
        CHECK(!surface.Render(host, "mp_chat.main", 640, 480, 0, invalid, output));
        CHECK(ImGui::GetCurrentContext() == main);
        // A clone may be queued on the render thread when its producer is destroyed.
        auto transient = std::make_unique<ui::PluginSurface>();
        CHECK(transient->Init(main));
        CHECK(transient->Render(host, "mp_chat.main", 640, 480, 1.0f / 60, {}, output));
        transient.reset();
        CHECK(output->data.Valid && output->data.TotalVtxCount > 0);
        CHECK(output->lists[0]->VtxBuffer.Size > 0);
        CHECK(output->data.Textures == &ImGui::GetPlatformIO().Textures);
        output.reset();
        CHECK(ImGui::GetCurrentContext() == main);
    }
    CHECK(ImGui::GetCurrentContext() == main);
    ImGui::DestroyContext(main);
    std::cout << "plugin_surface: PASS\n";
}
