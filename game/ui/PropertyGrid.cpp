#include "game/ui/PropertyGrid.hpp"

#include <charconv>
#include <cstring>
#include <sstream>

#include <imgui.h>

#include "engine/reflect/Visit.hpp"

namespace game {

namespace {

// ImGui has one active item at a time: a single in-progress edit cache is
// enough. Committing on deactivate keeps ONE undo step per interaction.
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

str valueToString(const reflect::Value& value) {
    // Round-trippable text (re-parsed by valueFromString): raw string, space-
    // separated vector components, no decoration. Exhaustive per kind.
    std::ostringstream out;
    reflect::visit(value, reflect::overloaded {
        [&](bool b)              { out << (b ? "true" : "false"); },
        [&](i32 x)               { out << x; },
        [&](u32 x)               { out << x; },
        [&](f32 x)               { out << x; },
        [&](f64 x)               { out << x; },
        [&](const str& s)        { out << s; },
        [&](const Vec2& v)       { out << v.x << " " << v.y; },
        [&](const Vec3& v)       { out << v.x << " " << v.y << " " << v.z; },
        [&](const Vec4& v)       { out << v.x << " " << v.y << " " << v.z << " " << v.w; },
        [&](const Quat& q)       { out << q.x << " " << q.y << " " << q.z << " " << q.w; },
        [&](const core::Guid& g) { out << g.toString(); },
    });
    return out.str();
}

std::optional<reflect::Value> valueFromString(reflect::FieldKind kind,
                                              const str& text) {
    using reflect::FieldKind;
    using reflect::Value;
    const auto floats = [&](u32 count) -> std::optional<std::array<f32, 4>> {
        std::istringstream in { text };
        std::array<f32, 4> out {};
        for (u32 i = 0; i < count; ++i) {
            if (!(in >> out[i])) {
                return std::nullopt;
            }
        }
        return out;
    };
    switch (kind) {
    case FieldKind::Bool:
        return Value { text == "true" || text == "1" };
    case FieldKind::I32:
        try { return Value { static_cast<i32>(std::stol(text)) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::U32:
        try { return Value { static_cast<u32>(std::stoul(text)) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::F32:
        try { return Value { std::stof(text) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::F64:
        try { return Value { std::stod(text) }; }
        catch (...) { return std::nullopt; }
    case FieldKind::Str:
        return Value { text };
    case FieldKind::Guid:
        if (const auto guid = core::Guid::fromString(text)) {
            return Value { *guid };
        }
        return std::nullopt;
    case FieldKind::Vec2:
        if (const auto v = floats(2)) {
            return Value { Vec2 { (*v)[0], (*v)[1] } };
        }
        return std::nullopt;
    case FieldKind::Vec3:
        if (const auto v = floats(3)) {
            return Value { Vec3 { (*v)[0], (*v)[1], (*v)[2] } };
        }
        return std::nullopt;
    case FieldKind::Vec4:
        if (const auto v = floats(4)) {
            return Value { Vec4 { (*v)[0], (*v)[1], (*v)[2], (*v)[3] } };
        }
        return std::nullopt;
    case FieldKind::Quat:
        if (const auto v = floats(4)) {
            Quat q;
            q.x = (*v)[0]; q.y = (*v)[1]; q.z = (*v)[2]; q.w = (*v)[3];
            return Value { q };
        }
        return std::nullopt;
    }
    return std::nullopt;
}

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

        // One widget per kind; adding a FieldKind without a widget is now a
        // compile error (was a silently un-editable field). Widgets edit a
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
            [&](const str&) { drawText(); },
            [&](const core::Guid&) { drawText(); },
        });
        ImGui::PopID();
    });
    ImGui::EndTable();
    return committed;
}

} // namespace game
