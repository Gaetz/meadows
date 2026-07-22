#include "game/ui/PropertyGrid.hpp"

#include <charconv>
#include <cstring>
#include <sstream>

#include <imgui.h>

#include "engine/reflect/Visit.hpp"
#include "game/ui/EventPicker.hpp"
#include "game/ui/FormPicker.hpp"
#include "game/ui/Keywords.hpp"

namespace game {

namespace {

// ImGui has one active item at a time: a single in-progress edit cache is
// enough. Committing on deactivate keeps ONE undo step per interaction.
// (This TU-local mutable is DELIBERATE — it mirrors ImGui's own
// single-active-item global model, and PropertyGrid is dev tooling, where
// §8's no-global rule for gameplay determinism does not bite.)
struct ActiveEdit {
    core::Guid form;
    u32 field { 0 };
    char text[512] {};
};
ActiveEdit gActive; // .field == 0 means inactive

bool isActive(const core::Guid& form, u32 field) {
    return gActive.field == field && gActive.form == form;
}

} // namespace

// (valueToString / valueFromString live in engine/reflect/ValueText —
// the CSV importer, the console and this grid share one codec.)

bool drawPropertyGrid(data::EditSession& session, const core::Guid& id) {
    const data::Form* form = session.view(id);
    const reflect::TypeInfo* type = session.viewType(id);
    if (!form || !type) {
        ImGui::TextDisabled("(no form selected)");
        return false;
    }

    bool committed = false;
    ImGui::Text("%s%s", type->name.c_str(),
                session.isDirty(id) ? " *" : "");
    ImGui::TextDisabled("%s", id.toString().c_str());
    ImGui::Separator();

    if (!ImGui::BeginTable("fields", 2,
                           ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    reflect::forEachField(*type, [&](const reflect::FieldInfo& field) {
        if (field.flags & reflect::Transient) {
            return;
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(field.name.c_str());
        ImGui::TableNextColumn();
        ImGui::PushID(static_cast<int>(field.id));
        ImGui::SetNextItemWidth(-1.0f);

        const reflect::Value current = field.get(form);
        const auto commit = [&](const reflect::Value& value) {
            if (session.setField(id, field.id, value)) {
                committed = true;
            }
        };
        // Commit a numeric/vector widget only once editing ends (one undo step).
        const auto commitOnDeactivate = [&](const reflect::Value& value) {
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                commit(value);
            }
        };
        // Str and Guid share a text field: inactive fields render from a LOCAL
        // copy of the committed value; the one active field types into the
        // shared cache; parse on deactivate (bad guids are dropped silently).
        const auto drawText = [&] {
            char local[512];
            const bool active = isActive(id, field.id);
            char* buffer = active ? gActive.text : local;
            if (!active) {
                const str text = valueToString(current);
                std::snprintf(local, sizeof(local), "%s", text.c_str());
            }
            ImGui::InputText("##v", buffer, sizeof(gActive.text));
            if (ImGui::IsItemActivated()) {
                gActive.form = id;
                gActive.field = field.id;
                std::memcpy(gActive.text, buffer, sizeof(gActive.text));
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (const auto value =
                        valueFromString(field.kind, gActive.text)) {
                    commit(*value);
                }
                gActive = {};
            } else if (ImGui::IsItemDeactivated()) {
                gActive = {};
            }
        };

        // One widget per kind; adding a FieldKind without a widget is a
        // compile error (not a silently un-editable field). Widgets edit a
        // local copy, then commit through the reflected setter (§2.9-clean).
        reflect::visit(current, reflect::overloaded {
            [&](bool b) {
                if (ImGui::Checkbox("##v", &b)) commit(reflect::Value { b });
            },
            [&](i32 v) {
                ImGui::InputInt("##v", &v);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](u32 v) {
                ImGui::InputScalar("##v", ImGuiDataType_U32, &v);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](f32 v) {
                ImGui::DragFloat("##v", &v, 0.05f);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](f64 v) {
                ImGui::InputDouble("##v", &v);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](Vec2 v) {
                ImGui::DragFloat2("##v", &v.x, 0.05f);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](Vec3 v) {
                ImGui::DragFloat3("##v", &v.x, 0.05f);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](Vec4 v) {
                ImGui::DragFloat4("##v", &v.x, 0.05f);
                commitOnDeactivate(reflect::Value { v });
            },
            [&](Quat q) {
                f32 v[4] = { q.x, q.y, q.z, q.w };
                ImGui::DragFloat4("##v", v, 0.01f);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    q.x = v[0]; q.y = v[1]; q.z = v[2]; q.w = v[3];
                    commit(reflect::Value { q });
                }
            },
            [&](const str& s) {
                // Closed vocabularies get a dropdown (keywords are not
                // typed by heart), Capitalized + blue;
                // the CANONICAL value is what commits. Event-name fields
                // get the scanned event vocabulary instead — an
                // OPEN list with inline creation.
                if (const auto* options =
                        keywordsFor(type->name, field.name)) {
                    str picked;
                    if (drawKeywordCombo("##v", *options, s, picked)) {
                        commit(reflect::Value { picked });
                    }
                } else if (isEventField(type->name, field.name)) {
                    str picked;
                    if (drawEventCombo("##v", session, s, picked)) {
                        commit(reflect::Value { picked });
                    }
                } else {
                    drawText();
                }
            },
            [&](const core::Guid& g) {
                // Item guid fields pick from the four item
                // categories; single-type guid fields (clip, cue
                // particles/sound, ability effects, schedule package)
                // get the typed picker. Raw text stays the fallback.
                if (isItemField(type->name, field.name)) {
                    core::Guid picked;
                    if (drawItemPicker("##v", session, g, picked)) {
                        commit(reflect::Value { picked });
                    }
                } else if (const auto* pickType =
                               pickerTypeFor(type->name, field.name)) {
                    core::Guid picked;
                    if (drawFormPicker("##v", session, pickType->id, g,
                                       picked)) {
                        commit(reflect::Value { picked });
                    }
                } else {
                    drawText();
                }
            },
        });
        ImGui::PopID();
    });
    ImGui::EndTable();
    return committed;
}

} // namespace game
