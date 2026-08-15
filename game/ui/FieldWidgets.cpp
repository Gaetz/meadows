#include "game/ui/FieldWidgets.hpp"

#include <cstring>

#include <imgui.h>

namespace game {

namespace {

// One active text edit at a time, mirroring ImGui's own
// single-active-item model — the grid and the structured panels share
// this ONE cache. (A deliberate TU-local mutable: dev tooling, so §8's
// no-global rule for gameplay determinism does not bite.)
struct ActiveEdit {
    core::Guid form;
    u32 field { 0 };
    char text[512] {};
};
ActiveEdit gEdit;

} // namespace

bool rawTextField(const core::Guid& id, u32 fieldId, const char* label,
                  const str& current, str& edited) {
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
        edited = gEdit.text;
        gEdit = {};
        return true;
    }
    if (ImGui::IsItemDeactivated()) {
        gEdit = {};
    }
    return false;
}

void textField(data::EditSession& session, const core::Guid& id,
               const char* label, u32 fieldId, const str& current) {
    str edited;
    if (rawTextField(id, fieldId, label, current, edited)) {
        session.setField(id, fieldId, reflect::Value { std::move(edited) });
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
