#include "game/ui/FieldWidgets.hpp"

#include <cstring>

#include <imgui.h>

namespace game {

namespace {

// One active text edit at a time — the PropertyGrid's deliberate
// TU-local pattern, shared by the structured panels.
struct ActiveEdit {
    core::Guid form;
    u32 field { 0 };
    char text[512] {};
};
ActiveEdit gEdit;

} // namespace

void textField(data::EditSession& session, const core::Guid& id,
               const char* label, u32 fieldId, const str& current) {
    const bool active = gEdit.field == fieldId && gEdit.form == id;
    char local[512];
    char* buffer = active ? gEdit.text : local;
    if (!active) {
        std::snprintf(local, sizeof(local), "%s", current.c_str());
    }
    ImGui::InputText(label, buffer, sizeof(gEdit.text));
    if (ImGui::IsItemActivated()) {
        gEdit.form = id;
        gEdit.field = fieldId;
        std::memcpy(gEdit.text, buffer, sizeof(gEdit.text));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        session.setField(id, fieldId, reflect::Value { str { gEdit.text } });
        gEdit = {};
    } else if (ImGui::IsItemDeactivated()) {
        gEdit = {};
    }
}

void floatField(data::EditSession& session, const core::Guid& id,
                const char* label, u32 fieldId, f32 current, f32 speed) {
    ImGui::DragFloat(label, &current, speed);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        session.setField(id, fieldId, reflect::Value { current });
    }
}

} // namespace game
