// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/platform/window.h"
#include "scuba/render/renderer.h"
#include "scuba/sco/sco_host.h"
#include "scuba/ui/plugin_surface.h"
#include "ui/ui_frame.h"
#include "imgui.h"
#include "sco/widgets.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)

int main(int argc, char** argv) {
    using namespace scuba;
    CHECK(argc >= 3);
    bool stress = false;
    bool pixelDeltaGate = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "stress") stress = true;
        else if (arg == "--pixel-delta-gate") pixelDeltaGate = true;
        else { std::fprintf(stderr, "unknown argument: %s\n", argv[i]); return 1; }
    }
    platform::Window window;
    platform::WindowDesc wd;
    wd.visible = false;
    CHECK(window.Create(wd));
    render::RendererDesc desc;
    desc.window = window.native(); desc.width = 1024; desc.height = 768;
    desc.offscreen = desc.warp = desc.debugLayer = true;
    Status status;
    auto renderer = render::Renderer::CreateD3D12(desc, status);
    CHECK(renderer && status);
    ImGuiContext* mainContext = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = {1024, 768};
    CHECK(renderer->InitUi());
    ScoHost host;
    ScoHostOptions options;
    options.pluginRoot = argv[1]; options.lua = false;
    CHECK(host.Start(options)); host.Tick();
    bool published = false;
    for (const auto& p : host.Panels())
        if (p.id == "mp_chat.main" && p.owner == "mp_chat") published = true;
    CHECK(published);
    std::fprintf(stderr, "published mp_chat.main owner mp_chat\n");
    std::array<std::unique_ptr<ui::PluginSurface>, 4> surfaces;
    std::array<render::GpuPanel, 4> panels{};
    const int count = stress ? 4 : 1;
    for (int i = 0; i < count; ++i) {
        surfaces[static_cast<size_t>(i)] = std::make_unique<ui::PluginSurface>();
        CHECK(surfaces[static_cast<size_t>(i)]->Init(mainContext));
        panels[static_cast<size_t>(i)] = renderer->CreatePanel(640, 480);
        CHECK(panels[static_cast<size_t>(i)]);
    }
    bool edited = false;
    int targetY = 20;
    std::vector<uint8_t> beforeEdit;
    uint32_t beforeWidth = 0, beforeHeight = 0;
    for (int frame = 0; frame < (stress ? 240 : 120); ++frame) {
        // Synthetic local input traverses the real ImGui adapter and owning plugin callback.
        renderer->BeginUiFrame();
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        ImGui::NewFrame();
        render::FramePacket packet;
        packet.view.viewport = {0, 0, 1024, 768}; packet.view.grid = false;
        for (int i = 0; i < count; ++i) {
            ui::SurfaceInput input;
            if (i == 0 && !edited && frame > 3) {
                input.pointer = input.focused = true;
                input.x = 80; input.y = static_cast<float>(targetY);
                const int phase = (frame - 4) % 3;
                input.buttons[0] = phase == 0;
                if (phase == 2) input.text = "VISUAL_ACCEPT_42";
            }
            render::PanelDraw draw;
            draw.panel = panels[static_cast<size_t>(i)];
            CHECK(surfaces[static_cast<size_t>(i)]->Render(host, "mp_chat.main", 640, 480,
                  1.0f / 60, input, draw.ui));
            CHECK(draw.ui && draw.ui->data.Valid);
            CHECK(draw.ui->data.CmdListsCount > 0 && draw.ui->data.TotalVtxCount > 0 &&
                  draw.ui->data.TotalIdxCount > 0);
            draw.world.m[0][0] = stress ? .9f : 1.6f;
            draw.world.m[1][1] = stress ? .9f : 1.6f;
            draw.world.m[3][0] = stress ? (i % 2 == 0 ? -.5f : .5f) : 0;
            draw.world.m[3][1] = stress ? (i < 2 ? .5f : -.5f) : 0;
            draw.world.m[3][2] = .5f;
            packet.panels.push_back(std::move(draw));
        }
        ImGui::Render();
        packet.ui.reset(new render::UiFrame());
        packet.ui->data = *ImGui::GetDrawData();
        packet.ui->data.CmdLists.resize(0);
        renderer->Submit(std::move(packet));
        host.Tick();
        for (const auto& p : sco::widgets::Panels())
            if (p.id == "mp_chat.main") for (const auto& node : p.nodes)
                if (node.kind == SCO_WIDGET_TEXT_INPUT && node.text == "VISUAL_ACCEPT_42") edited = true;
        if (!edited && frame > 3 && (frame - 4) % 3 == 2) targetY += 16;
        if (frame == 3) {
            // Save the same scene before any synthetic input for an experimental diagnostic.
            CHECK(renderer->Readback(beforeEdit, beforeWidth, beforeHeight));
        }
        if (stress && (frame == 80 || frame == 160)) {
            renderer->Flush();
            renderer->Resize(frame == 80 ? 1280u : 1024u, frame == 80 ? 960u : 768u);
            // Retire all four in-flight targets and reuse their descriptor slots.
            for (auto& gpuPanel : panels) {
                renderer->ReleasePanel(gpuPanel);
                gpuPanel = renderer->CreatePanel(640, 480);
                CHECK(gpuPanel);
            }
        }
    }
    CHECK(edited);
    std::fprintf(stderr, "input routed to plugin-owned TEXT_INPUT: VISUAL_ACCEPT_42\n");
    renderer->Flush();
    std::vector<uint8_t> rgba; uint32_t width = 0, height = 0;
    CHECK(renderer->Readback(rgba, width, height));
    CHECK(width == 1024 && height == 768 && width == beforeWidth && height == beforeHeight);
    CHECK(rgba.size() == beforeEdit.size());
    CHECK(rgba.size() == static_cast<size_t>(width) * height * 4);
    // Identity view/projection and the fixture transforms put the quads at 10..90%
    // for one surface, or 2.5..47.5% and 52.5..97.5% on each axis for four.
    // Exclude the boundaries and report RGB changes inside every quad. The threshold is
    // experimental: only enforce it when explicitly requested, until calibrated on Windows.
    // Changes can have other causes and are not proof of readable glyphs.
    for (int i = 0; i < count; ++i) {
        const uint32_t x0 = stress ? (i % 2 == 0 ? 35u : 535u) : 110u;
        const uint32_t x1 = stress ? (i % 2 == 0 ? 465u : 965u) : 890u;
        const uint32_t y0 = stress ? (i < 2 ? 35u : 535u) : 110u;
        const uint32_t y1 = stress ? (i < 2 ? 465u : 965u) : 890u;
        size_t changed = 0;
        for (uint32_t y = height * y0 / 1000; y < height * y1 / 1000; ++y) {
            for (uint32_t x = width * x0 / 1000; x < width * x1 / 1000; ++x) {
                const size_t p = (static_cast<size_t>(y) * width + x) * 4;
                if (std::abs(int{rgba[p]} - int{beforeEdit[p]}) > 16 ||
                    std::abs(int{rgba[p + 1]} - int{beforeEdit[p + 1]}) > 16 ||
                    std::abs(int{rgba[p + 2]} - int{beforeEdit[p + 2]}) > 16) ++changed;
            }
        }
        std::fprintf(stderr, "surface %d: %zu changed RGB pixels after plugin edit\n", i, changed);
        if (pixelDeltaGate) CHECK(changed >= 20);
    }
    std::FILE* out = std::fopen(argv[2], "wb"); CHECK(out);
    std::fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (size_t i = 0; i < rgba.size(); i += 4) CHECK(std::fwrite(rgba.data() + i, 1, 3, out) == 3);
    CHECK(std::fclose(out) == 0);
    for (int i = 0; i < count; ++i) renderer->ReleasePanel(panels[static_cast<size_t>(i)]);
    renderer->Flush();
    for (auto& surface : surfaces) surface.reset();
    host.Stop();
    renderer->ShutdownUi();
    ImGui::DestroyContext(mainContext);
    std::fprintf(stderr, "plugin_visual PASS: %d surfaces, %s\n", count, argv[2]);
}
