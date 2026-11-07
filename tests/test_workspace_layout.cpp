// SPDX-License-Identifier: GPL-3.0-only
#include "check.h"

#include "ui/workspace_layout.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace scuba::ui::workspace;

int main() {
    const auto missingPath = std::filesystem::temp_directory_path() /
        ("scuba-engine-layout-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    CHECK(ShouldBuildDefaultWorkspace(missingPath));
    CHECK(!ShouldBuildDefaultWorkspace(std::filesystem::temp_directory_path()));
    const auto savedPath = missingPath.string() + ".saved";
    {
        std::ofstream savedLayout(savedPath, std::ios::binary);
        CHECK(savedLayout.good());
    }
    CHECK(!ShouldBuildDefaultWorkspace(savedPath));
    std::filesystem::remove(savedPath);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    ImGui::NewFrame();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiID expectedRoot = MainDockSpaceId(viewport->ID);
    CHECK(BuildDefaultWorkspace(expectedRoot, viewport->WorkPos, ImVec2(0.0f, viewport->WorkSize.y)).root == 0);
    const auto nodes = BuildDefaultWorkspace(expectedRoot, viewport->WorkPos, viewport->WorkSize);
    CHECK(nodes.root == expectedRoot);
    CHECK(nodes.scene != 0 && nodes.inspector != 0 && nodes.content != 0 && nodes.viewport != 0);
    CHECK(ImGui::DockBuilderGetNode(nodes.scene)->SelectedTabId == ImHashStr("Scene"));
    CHECK(ImGui::DockBuilderGetNode(nodes.inspector)->SelectedTabId == ImHashStr("Inspector"));
    CHECK(ImGui::DockBuilderGetNode(nodes.content)->SelectedTabId == ImHashStr("Content"));

    const ImGuiID submittedRoot = ImGui::DockSpaceOverViewport(
        0, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
    CHECK(submittedRoot == expectedRoot);
    CHECK(ImGui::DockBuilderGetCentralNode(submittedRoot)->ID == nodes.viewport);

    auto* sceneSettings = ImGui::FindWindowSettingsByID(ImHashStr("Scene"));
    auto* mountsSettings = ImGui::FindWindowSettingsByID(ImHashStr("Mounts"));
    auto* inspectorSettings = ImGui::FindWindowSettingsByID(ImHashStr("Inspector"));
    auto* pluginSettings = ImGui::FindWindowSettingsByID(ImHashStr("Plugins"));
    auto* contentSettings = ImGui::FindWindowSettingsByID(ImHashStr("Content"));
    auto* loaderSettings = ImGui::FindWindowSettingsByID(ImHashStr("Asset loader"));
    auto* logSettings = ImGui::FindWindowSettingsByID(ImHashStr("Log"));
    auto* statsSettings = ImGui::FindWindowSettingsByID(ImHashStr("Stats"));
    CHECK(sceneSettings && sceneSettings->DockId == nodes.scene);
    CHECK(mountsSettings && mountsSettings->DockId == nodes.scene);
    CHECK(inspectorSettings && inspectorSettings->DockId == nodes.inspector);
    CHECK(pluginSettings && pluginSettings->DockId == nodes.inspector);
    CHECK(contentSettings && contentSettings->DockId == nodes.content);
    CHECK(loaderSettings && loaderSettings->DockId == nodes.content);
    CHECK(logSettings && logSettings->DockId == nodes.content);
    CHECK(statsSettings && statsSettings->DockId == nodes.content);

    ImGui::Begin("Scene"); ImGui::TextUnformatted("outliner"); ImGui::End();
    ImGui::Begin("Mounts"); ImGui::TextUnformatted("mounts"); ImGui::End();
    ImGui::Begin("Inspector"); ImGui::TextUnformatted("inspector"); ImGui::End();
    ImGui::Begin("Plugins"); ImGui::TextUnformatted("plugins"); ImGui::End();
    ImGui::Begin("Content"); ImGui::TextUnformatted("assets"); ImGui::End();
    ImGui::Begin("Asset loader"); ImGui::TextUnformatted("loader"); ImGui::End();
    ImGui::Begin("Log"); ImGui::TextUnformatted("log"); ImGui::End();
    ImGui::Begin("Stats"); ImGui::TextUnformatted("stats"); ImGui::End();
    ImGui::Render();
    CHECK(ImGui::DockBuilderGetNode(nodes.scene)->SelectedTabId == ImHashStr("Scene"));
    CHECK(ImGui::DockBuilderGetNode(nodes.inspector)->SelectedTabId == ImHashStr("Inspector"));
    CHECK(ImGui::DockBuilderGetNode(nodes.content)->SelectedTabId == ImHashStr("Content"));

    ImGui::DestroyContext();
    return CheckExit("test_workspace_layout");
}
