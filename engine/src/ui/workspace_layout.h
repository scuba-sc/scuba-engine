// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "imgui.h"

#include <filesystem>

namespace scuba::ui::workspace {

inline constexpr char kLayoutIniFilename[] = "scuba-engine-layout.ini";

struct DefaultWorkspaceNodes {
    ImGuiID root = 0;
    ImGuiID scene = 0;
    ImGuiID inspector = 0;
    ImGuiID content = 0;
    ImGuiID viewport = 0;
};

// A saved settings file wins even when it is empty or cannot be parsed: do not replace a user's
// workspace just because the application cannot recognize its contents.
bool ShouldBuildDefaultWorkspace(const std::filesystem::path& settingsPath);

// This reproduces DockSpaceOverViewport(0, viewport)'s root ID without submitting its host window,
// allowing ImGui's DockBuilder calls to run before the dockspace is submitted for the frame.
ImGuiID MainDockSpaceId(ImGuiID viewportId);

DefaultWorkspaceNodes BuildDefaultWorkspace(ImGuiID root, ImVec2 position, ImVec2 size);

}  // namespace scuba::ui::workspace
