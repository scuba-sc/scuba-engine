// SPDX-License-Identifier: GPL-3.0-only
#include "ui/workspace_layout.h"

#include "imgui_internal.h"

#include <cmath>
#include <system_error>

namespace scuba::ui::workspace {

bool ShouldBuildDefaultWorkspace(const std::filesystem::path& settingsPath) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(settingsPath, ec);
    return !ec && !exists;
}

ImGuiID MainDockSpaceId(ImGuiID viewportId) {
    char hostName[32]{};
    ImFormatString(hostName, IM_COUNTOF(hostName), "WindowOverViewport_%08X", viewportId);
    const ImGuiID hostWindowId = ImHashStr(hostName);
    return ImHashStr("DockSpace", 0, hostWindowId);
}

DefaultWorkspaceNodes BuildDefaultWorkspace(ImGuiID root, ImVec2 position, ImVec2 size) {
    DefaultWorkspaceNodes result;
    if (root == 0 || !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0.0f || size.y <= 0.0f)
        return result;

    ImGui::DockBuilderRemoveNode(root);
    ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(root, position);
    ImGui::DockBuilderSetNodeSize(root, size);

    ImGuiID remaining = root;
    result.scene = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Left, 0.22f, nullptr, &remaining);
    result.inspector = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, 0.24f, nullptr, &remaining);
    result.content = ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Down, 0.28f, nullptr, &remaining);
    result.viewport = remaining;
    result.root = root;

    // Related panels share tabs so the central viewport remains the primary workspace. Initial
    // selections are set below so the outliner, inspector, and asset browser are immediately seen.
    ImGui::DockBuilderDockWindow("Scene", result.scene);
    ImGui::DockBuilderDockWindow("Mounts", result.scene);
    ImGui::DockBuilderDockWindow("Inspector", result.inspector);
    ImGui::DockBuilderDockWindow("Plugins", result.inspector);
    ImGui::DockBuilderDockWindow("Content", result.content);
    ImGui::DockBuilderDockWindow("Asset loader", result.content);
    ImGui::DockBuilderDockWindow("Log", result.content);
    ImGui::DockBuilderDockWindow("Stats", result.content);
    ImGui::DockBuilderFinish(root);

    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(result.scene))
        node->SelectedTabId = ImHashStr("Scene");
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(result.inspector))
        node->SelectedTabId = ImHashStr("Inspector");
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(result.content))
        node->SelectedTabId = ImHashStr("Content");
    return result;
}

}  // namespace scuba::ui::workspace
