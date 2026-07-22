#include "game/ui/ClipTimelinePanel.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <imgui.h>

#include "data/forms/AnimForms.hpp"

namespace game {

void ClipTimelinePanel::drawEditor(const core::Guid& clipId) {
    const auto* type = clipId.isValid() ? session.viewType(clipId) : nullptr;
    if (!type || type->id != data::AnimClipForm::staticTypeInfo().id) {
        ImGui::TextDisabled("(select an anim clip in the Browser)");
        return;
    }
    const auto* clip =
        static_cast<const data::AnimClipForm*>(session.view(clipId));

    // The clip's events, sorted by time (then guid: stable at equal times).
    vector<std::pair<core::Guid, const data::AnimEventForm*>> events;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& formType) {
        if (formType.id != data::AnimEventForm::staticTypeInfo().id) {
            return;
        }
        const auto* event = static_cast<const data::AnimEventForm*>(&form);
        if (event->parent == clipId) {
            events.emplace_back(id, event);
        }
    });
    std::sort(events.begin(), events.end(),
              [](const auto& a, const auto& b) {
                  return a.second->time != b.second->time
                             ? a.second->time < b.second->time
                             : a.first < b.first;
              });

    // View length: auto from the events (the real duration lives in the
    // glTF — a known, accepted gap), overridable.
    f32 maxTime = 0.0f;
    for (const auto& [id, event] : events) {
        maxTime = glm::max(maxTime, event->time);
    }
    const f32 autoLength = glm::max(1.0f, maxTime * 1.25f);
    f32 length = viewLength > 0.0f ? viewLength : autoLength;

    ImGui::Text("%s", clip->editorId.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu events — drag markers, click a gap to add",
                        events.size());
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat("view (s)", &length, 0.05f, 0.2f, 60.0f, "%.2f")) {
        viewLength = length;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("auto")) {
        viewLength = 0.0f;
        length = autoLength;
    }

    // The strip.
    constexpr f32 kStripHeight = 64.0f;
    constexpr f32 kGridTop = 16.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 width = glm::max(ImGui::GetContentRegionAvail().x, 240.0f);
    const auto xAt = [&](f32 t) { return origin.x + t / length * width; };
    const auto timeAt = [&](f32 x) {
        const f32 raw = (x - origin.x) / width * length;
        return glm::clamp(std::round(raw * 100.0f) / 100.0f, 0.0f, length);
    };
    // Second/tenth grid, adaptive: aim for ~10 labelled ticks.
    const f32 step = length > 5.0f ? 1.0f : (length > 1.5f ? 0.5f : 0.1f);
    for (f32 t = 0.0f; t <= length + 1e-4f; t += step) {
        const f32 x = xAt(t);
        draw->AddLine(ImVec2(x, origin.y + kGridTop - 4.0f),
                      ImVec2(x, origin.y + kStripHeight),
                      IM_COL32(120, 120, 120, 120));
        char label[16];
        std::snprintf(label, sizeof(label), "%.2gs", t);
        draw->AddText(ImVec2(x + 2.0f, origin.y),
                      IM_COL32(160, 160, 160, 255), label);
    }
    draw->AddRectFilled(ImVec2(origin.x, origin.y + kGridTop),
                        ImVec2(origin.x + width, origin.y + kStripHeight),
                        IM_COL32(255, 255, 255, 10));

    // Markers: label rows alternate so neighbours stay readable.
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& [eventId, event] = events[i];
        f32 time = event->time;
        if (dragEvent == eventId) {
            time = dragTime; // live preview; the field commits on release
        }
        const f32 x = xAt(time);
        const bool isSelected = selected == eventId;
        const u32 color = isSelected ? IM_COL32(255, 200, 80, 255)
                                     : IM_COL32(120, 190, 255, 255);
        draw->AddLine(ImVec2(x, origin.y + kGridTop),
                      ImVec2(x, origin.y + kStripHeight), color, 2.0f);
        const f32 labelY =
            origin.y + kGridTop + 4.0f + (i % 2 == 0 ? 0.0f : 16.0f);
        draw->AddText(ImVec2(x + 4.0f, labelY), color,
                      event->name.empty() ? "(unnamed)"
                                          : event->name.c_str());

        // Drag handle: 0.01 s snap, ONE undoable edit on release.
        ImGui::SetCursorScreenPos(ImVec2(x - 5.0f, origin.y + kGridTop));
        ImGui::InvisibleButton(("##ev" + eventId.toString()).c_str(),
                               ImVec2(10.0f, kStripHeight - kGridTop));
        if (ImGui::IsItemActivated()) {
            dragEvent = eventId;
            dragTime = event->time;
            selected = eventId;
        }
        if (ImGui::IsItemActive() && dragEvent == eventId) {
            dragTime = timeAt(ImGui::GetMousePos().x);
        }
        if (ImGui::IsItemDeactivated() && dragEvent == eventId) {
            session.setField(eventId, core::fnv1a("time"),
                             reflect::Value { dragTime });
            dragEvent = {};
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }

    // The empty strip: click = "+ event" at that time.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + kGridTop));
    ImGui::InvisibleButton("##strip", ImVec2(width,
                                             kStripHeight - kGridTop));
    if (ImGui::IsItemClicked()) {
        data::EditSession::Gesture gesture { session };
        const f32 time = timeAt(ImGui::GetMousePos().x);
        const core::Guid id = session.createForm(
            data::AnimEventForm::staticTypeInfo().id,
            clip->editorId + "Event" + std::to_string(++createCounter));
        session.setField(id, core::fnv1a("parent"),
                         reflect::Value { clipId });
        session.setField(id, core::fnv1a("time"), reflect::Value { time });
        session.setField(id, core::fnv1a("name"), reflect::Value {
                                                      str { "Event" } });
        selected = id;
    }
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x, origin.y + kStripHeight + 4.0f));
    ImGui::Dummy(ImVec2(width, 4.0f));
    ImGui::TextDisabled(
        "The clip's REAL duration lives in the glTF — 'view (s)' is only "
        "the ruler.");
}

} // namespace game
