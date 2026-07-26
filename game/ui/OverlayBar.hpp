#pragma once

#include <imgui.h>

#include "engine/core/Defines.hpp"

namespace game {

// The ONE overlay loading bar (startup gate, tree generation…): drawn
// straight on a drawlist so it layers over the black gate and over the
// live scene alike — every loading UI shares this look. `label` (may be
// null) is centered under the bar; `alpha` rides an owning fade.
inline void drawOverlayBar(ImDrawList* draw, const ImVec2& center,
                           f32 width, f32 progress, const char* label,
                           f32 alpha = 1.0f) {
    const f32 filled = width * (progress < 0.0f   ? 0.0f
                                : progress > 1.0f ? 1.0f
                                                  : progress);
    const ImVec2 lo { center.x - width * 0.5f, center.y - 4.0f };
    const ImVec2 hi { center.x + width * 0.5f, center.y + 4.0f };
    draw->AddRectFilled(
        lo, hi, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.12f * alpha)),
        4.0f);
    draw->AddRectFilled(
        lo, { lo.x + filled, hi.y },
        ImGui::GetColorU32(ImVec4(0.85f, 0.9f, 1.0f, 0.85f * alpha)), 4.0f);
    if (label != nullptr && label[0] != '\0') {
        const ImVec2 size = ImGui::CalcTextSize(label);
        draw->AddText(
            { center.x - size.x * 0.5f, hi.y + 10.0f },
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.8f * alpha)),
            label);
    }
}

} // namespace game
