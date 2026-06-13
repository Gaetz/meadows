#include "game/scenes/DemoScenes.hpp"

#include <imgui.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/combat/Combat.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

const core::Guid kStrikeAbility =
    *core::Guid::fromString("ab000000-0000-4000-8000-000000000001");

} // namespace

// --- PluginScene ------------------------------------------------------------

void PluginScene::drawUi() {
    ImGui::Begin("Plugins / mods (data model)");
    if (modPlugin &&
        ImGui::Checkbox("Enable 'golden-blades' mod", &modEnabled)) {
        rebuild(); // full §5 re-resolution
    }
    ImGui::TextWrapped(
        "The mod patches the sword (gold + stronger), moves Sword_A and "
        "disables Sword_B - all field-level patches on records.");
    ImGui::Separator();
    ImGui::Text("%u forms, %zu cells, %zu conflicts", forms.count(),
                model.cells().size(), report.conflicts.size());
    for (const data::FieldConflict& conflict : report.conflicts) {
        ImGui::BulletText("%s.%s", conflict.typeName.c_str(),
                          conflict.fieldName.c_str());
    }
    ImGui::End();
}

// --- WorldEditScene ---------------------------------------------------------

void WorldEditScene::onEnter() {
    WorldDemoScene::onEnter();
    editor = std::make_unique<WorldEditor>(world, forms, categories, spawner);
    if (!model.cells().empty()) {
        const data::FormHandle cellHandle = model.cells().front();
        const data::Form* cellForm = forms.get(cellHandle);
        editor->setActiveCell(cellLoader->cellEntity(cellHandle),
                              cellForm ? cellForm->id : core::Guid {});
    }
}

void WorldEditScene::drawUi() {
    if (editor) {
        editor->drawUi();
    }
}

// --- CombatScene ------------------------------------------------------------

void CombatScene::drawUi() {
    const gameplay::AbilityForm* strike =
        forms.find<gameplay::AbilityForm>(kStrikeAbility);
    const auto deadTag = tags.find("State.Dead");

    ImGui::Begin("Combat (GAS debug)");
    if (!strike) {
        ImGui::TextDisabled("No Strike ability in the database.");
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("'Strike' applies a 10-damage effect (1s cooldown). "
                       "Health 0 grants State.Dead.");
    ImGui::Separator();

    world.handle()
        .query<gameplay::AttributeSet, gameplay::AbilitySystem>()
        .each([&](flecs::entity entity, gameplay::AttributeSet& set,
                  gameplay::AbilitySystem& system) {
            str name = "actor";
            if (const world::RefId* refId = entity.try_get<world::RefId>()) {
                if (const data::Form* base = forms.get(refId->base);
                    base && !base->editorId.empty()) {
                    name = base->editorId;
                }
            }
            const f32 health =
                gameplay::currentValueOf(system, gameplay::attr("health"));
            const f32 maxHealth =
                gameplay::currentValueOf(system, gameplay::attr("maxHealth"));
            const bool dead = deadTag && system.tags.has(*deadTag);

            ImGui::PushID(static_cast<int>(entity.id()));
            ImGui::Text("%s%s  %.0f / %.0f", name.c_str(),
                        dead ? " (DEAD)" : "", health, maxHealth);
            ImGui::ProgressBar(maxHealth > 0.0f ? health / maxHealth : 0.0f,
                               ImVec2(-1.0f, 0.0f));
            if (ImGui::Button("Strike")) {
                gameplay::performAttack(*strike, set, system, set, system,
                                        { forms, tags });
            }
            ImGui::PopID();
            ImGui::Separator();
        });
    ImGui::End();
}

} // namespace game
