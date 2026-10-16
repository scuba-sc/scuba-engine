// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// An ImGui frame handed from the main thread to the render thread: ImDrawData with its draw lists
// deep-copied, so the main thread can build the next frame while this one is recorded. Texture
// update requests (draw_data->Textures) are shared with the context; the renderer processes them
// before it signals "UI consumed", and BeginUiFrame waits for that signal.
#include "imgui.h"

#include <vector>

namespace scuba::render {

struct UiFrame {
    ImDrawData data;
    std::vector<ImDrawList*> lists;   // owned clones

    ~UiFrame() {
        for (ImDrawList* l : lists) IM_DELETE(l);
    }
};

}  // namespace scuba::render
