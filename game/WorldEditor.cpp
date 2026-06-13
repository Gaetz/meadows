#include "game/WorldEditor.hpp"

#include <cmath>
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <imgui.h>

#include "world/scene/Components.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

void WorldEditor::drawUi() {
    ImGui::Begin("World editor");

    // --- Palette: every spawnable base form (those that have a category) ---
    vector<core::Guid> paletteGuids;
    vector<str> paletteLabels;
    for (u32 value = 1; value <= forms.count(); ++value) {
        const data::FormHandle handle { value };
        const reflect::TypeInfo* type = forms.typeOf(handle);
        const data::Form* form = forms.get(handle);
        if (!type || !form || !categories.categoryOf(type->id)) {
            continue;
        }
        paletteGuids.push_back(form->id);
        paletteLabels.push_back(form->editorId.empty() ? form->id.toString()
                                                       : form->editorId);
    }

    ImGui::SeparatorText("Add object");
    if (paletteGuids.empty()) {
        ImGui::TextDisabled("No spawnable base forms in the database.");
    } else {
        if (paletteIndex >= static_cast<int>(paletteGuids.size())) {
            paletteIndex = 0;
        }
        vector<const char*> items;
        items.reserve(paletteLabels.size());
        for (const str& label : paletteLabels) {
            items.push_back(label.c_str());
        }
        ImGui::Combo("base form", &paletteIndex, items.data(),
                     static_cast<int>(items.size()));
        ImGui::DragFloat2("position", placePosition, 0.05f);

        const bool canAdd = activeCell.is_valid();
        if (!canAdd) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Add to active cell")) {
            world::ReferenceForm reference;
            reference.id = core::Guid::generate();
            reference.editorId = "PlacedObject";
            reference.baseForm = paletteGuids[paletteIndex];
            reference.cell = activeCellId;
            reference.position = { placePosition[0], placePosition[1], 0.0f };
            world::SpawnContext ctx { world, forms, categories };
            const ecs::Entity entity = spawner.spawn(ctx, reference, activeCell);
            if (entity.is_alive()) {
                selected = entity;
            }
        }
        if (!canAdd) {
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(no active cell)");
        }
    }

    // --- Object list: pick one to edit ---
    ImGui::SeparatorText("Objects");
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity entity, const world::RefId& refId) {
            const data::Form* base = forms.get(refId.base);
            str label = base && !base->editorId.empty() ? base->editorId
                                                        : str { "<object>" };
            label += "##" + std::to_string(entity.id());
            if (ImGui::Selectable(label.c_str(), entity == selected)) {
                selected = entity;
            }
        });

    // --- Selected object: move / rotate / delete (instance state, §2.2) ---
    ImGui::SeparatorText("Selected");
    if (selected.is_alive() && selected.has<world::Transform>()) {
        world::Transform transform = selected.get<world::Transform>();
        if (ImGui::DragFloat2("pos", &transform.position.x, 0.05f)) {
            selected.set<world::Transform>(transform);
        }
        float yaw = glm::degrees(
            2.0f * std::atan2(transform.rotation.z, transform.rotation.w));
        if (ImGui::DragFloat("yaw", &yaw, 1.0f)) {
            transform.rotation =
                glm::angleAxis(glm::radians(yaw), Vec3 { 0.0f, 0.0f, 1.0f });
            selected.set<world::Transform>(transform);
        }
        if (ImGui::Button("Delete")) {
            selected.destruct();
            selected = ecs::Entity {};
        }
    } else {
        ImGui::TextDisabled("Select an object in the list above.");
    }

    ImGui::End();
}

} // namespace game
