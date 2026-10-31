#include "check.h"
#include "scuba/ui/net_sandbox.h"
#include <chrono>
#include <iostream>
#include "imgui.h"

using scuba::ui::NetSandbox;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
    CHECK(argc == 2);
    NetSandbox dock(argv[1]);
    CHECK(dock.Read().peers.empty());
    CHECK(!dock.Start(1, "", true));
    CHECK(!dock.Start(9, "", true));
    CHECK(!dock.Start(2, std::string(1024, 'x'), true));
    CHECK(!dock.Join(1));
    CHECK(!dock.Leave(1));
    CHECK(!dock.Send(0, 16));
    CHECK(!dock.Send(1, 0));
    CHECK(!dock.Send(1, 101));
    CHECK(!dock.Read().busy);
    std::cout << "native netsandbox: bounds passed\n";
    CHECK(dock.Start(2, "", true));
    CHECK(!dock.Start(2, "", true));
    CHECK(dock.WaitIdle(60s));
    auto state = dock.Read();
    if (!state.error.empty()) std::cerr << state.error << '\n';
    CHECK(state.error.empty());
    CHECK(state.peers.size() == 2);
    CHECK(state.peers[0].slot == 1);
    CHECK(state.peers[0].active);
    CHECK(state.peers[0].members == 2);
    CHECK(state.peers[0].port > 0);
    CHECK(state.peers[0].hasProbe);
    CHECK(state.peers[0].pid != state.peers[1].pid);
    CHECK(dock.Send(1, 16));
    CHECK(dock.WaitIdle(60s));
    CHECK(dock.Read().peers[0].reliableSent == 16);
    CHECK(dock.Read().peers[0].unreliableSent == 16);
    CHECK(dock.Read().peers[0].hasTraffic);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(320, 240);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        dock.Draw();
        ImGui::Render();
    }
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    ImGui::DestroyContext();
    CHECK(dock.WaitIdle(60s));
    CHECK(!dock.Start(2, "", true));
    CHECK(!dock.Join(2));
    CHECK(!dock.Leave(3));
    CHECK(dock.Leave(2));
    CHECK(dock.WaitIdle(60s));
    CHECK(dock.Read().peers.size() == 1);
    CHECK(dock.Join(2));
    CHECK(dock.WaitIdle(60s));
    CHECK(dock.Read().peers.size() == 2);
    CHECK(dock.Refresh());
    CHECK(dock.WaitIdle(60s));
    CHECK(dock.Read().error.empty());
    CHECK(dock.Stop());
    CHECK(dock.WaitIdle(60s));
    CHECK(dock.Read().peers.empty());
    CHECK(!dock.Send(1, 16));
    std::cout << "native netsandbox: real broker lifecycle passed\n";
    CHECK(dock.Start(2, "", false));
    CHECK(dock.WaitIdle(60s));
    CHECK(dock.Read().peers.size() == 2);
    CHECK(!dock.Read().peers[0].hasProbe);
    CHECK(!dock.Send(1, 16));
    CHECK(dock.Stop());
    CHECK(dock.WaitIdle(60s));
    NetSandbox missing("/nonexistent/scuba-hostd");
    CHECK(missing.Start(2, "", true));
    CHECK(missing.WaitIdle(5s));
    CHECK(!missing.Read().error.empty());
    CHECK(missing.Read().stale);
    CHECK(missing.Refresh());
    CHECK(missing.WaitIdle(5s));
    CHECK(missing.Read().error.empty());
    CHECK(!missing.Read().stale);
    return CheckExit("native_net_sandbox");
}
