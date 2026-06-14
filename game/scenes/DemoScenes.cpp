#include "game/scenes/DemoScenes.hpp"

#include <cstdio>

#include <imgui.h>

#include "engine/platform/Input.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "world/ai/AiController.hpp"
#include "world/scene/Collision.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Movement.hpp"

namespace game {

namespace {

const core::Guid kStrikeAbility =
    *core::Guid::fromString("ab000000-0000-4000-8000-000000000001");

// Placeholder player sprite (the iron sword texture, tinted), until the player
// has its own asset.
const core::Guid kPlayerSprite =
    *core::Guid::fromString("7c5e2d90-4f3a-4b18-a52e-c91d80f64b23");

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

    // The cooldown is a duration effect granting a tag; resolve its total length
    // and tag so we can draw a depleting bar from each actor's active effect.
    const gameplay::EffectForm* cooldownEffect =
        strike && strike->cooldown.isValid()
            ? forms.find<gameplay::EffectForm>(strike->cooldown)
            : nullptr;
    const auto cooldownTag =
        cooldownEffect && !cooldownEffect->grantedTag.empty()
            ? tags.find(cooldownEffect->grantedTag)
            : std::nullopt;
    const f32 cooldownTotal =
        cooldownEffect ? cooldownEffect->durationSeconds : 0.0f;

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

            // Cooldown: remaining time of the active effect carrying the
            // cooldown tag, as a fraction of its total — a bar that empties.
            f32 cooldownRemaining = 0.0f;
            if (cooldownTag && system.tags.has(*cooldownTag)) {
                for (const gameplay::ActiveEffect& active : system.activeEffects) {
                    if (active.grantedTag == *cooldownTag) {
                        cooldownRemaining = active.remaining;
                        break;
                    }
                }
            }
            const bool onCooldown = cooldownRemaining > 0.0f;

            if (onCooldown) {
                ImGui::BeginDisabled();
                ImGui::Button("Strike");
                ImGui::EndDisabled();
                ImGui::SameLine();
                const f32 fraction =
                    cooldownTotal > 0.0f ? cooldownRemaining / cooldownTotal : 0.0f;
                char overlay[16];
                std::snprintf(overlay, sizeof(overlay), "%.1fs",
                              cooldownRemaining);
                ImGui::ProgressBar(fraction, ImVec2(120.0f, 0.0f), overlay);
            } else if (ImGui::Button("Strike")) {
                gameplay::performAttack(*strike, set, system, set, system,
                                        { forms, tags });
            }
            ImGui::PopID();
            ImGui::Separator();
        });
    ImGui::End();
}

// --- GameplayScene ----------------------------------------------------------

void GameplayScene::onEnter() {
    WorldDemoScene::onEnter();
    ai::registerAiComponents(world);
    gameplay::registerInventoryComponents(world);

    // The player: controllable, collides, carries items.
    player = world.create();
    world::Transform transform;
    transform.position = { 0.0f, -2.0f, 0.0f };
    player.set<world::Transform>(transform);
    world::SpriteRender sprite;
    sprite.sprite = kPlayerSprite;
    sprite.tint = { 0.4f, 0.6f, 1.0f, 1.0f }; // blue, to stand out
    sprite.layer = 1;
    player.set<world::SpriteRender>(sprite);
    player.set<world::Velocity>({});
    player.set<world::Collider>({ Vec2 { 0.4f, 0.4f }, false });
    player.set<gameplay::Inventory>({});
    player.set<gameplay::Equipment>({});

    // Give spawned actors a chase AI + a solid body; items a pickup trigger.
    // (Collect first, then mutate — no structural change mid-query.)
    vector<ecs::Entity> actors;
    vector<ecs::Entity> items;
    world.handle().query_builder().with<world::ActorMarker>().build().each(
        [&](flecs::entity entity) { actors.push_back(entity); });
    world.handle().query_builder().with<world::ItemMarker>().build().each(
        [&](flecs::entity entity) { items.push_back(entity); });
    for (ecs::Entity actor : actors) {
        actor.set<ai::AiAgent>({});
        actor.set<world::Velocity>({});
        actor.set<world::Collider>({ Vec2 { 0.5f, 0.5f }, false });
    }
    for (ecs::Entity item : items) {
        item.set<world::Collider>({ Vec2 { 0.6f, 0.6f }, true }); // trigger
    }
}

void GameplayScene::update(f32 dt) {
    WorldDemoScene::update(dt); // GAS tick on the world's actors

    // Player input → velocity.
    const platform::Input& input = engine.getInput();
    Vec3 direction { 0.0f, 0.0f, 0.0f };
    if (input.isDown(platform::Key::W) || input.isDown(platform::Key::Up)) {
        direction.y += 1.0f;
    }
    if (input.isDown(platform::Key::S) || input.isDown(platform::Key::Down)) {
        direction.y -= 1.0f;
    }
    if (input.isDown(platform::Key::D) || input.isDown(platform::Key::Right)) {
        direction.x += 1.0f;
    }
    if (input.isDown(platform::Key::A) || input.isDown(platform::Key::Left)) {
        direction.x -= 1.0f;
    }
    if (direction.x != 0.0f || direction.y != 0.0f) {
        direction = glm::normalize(direction);
    }
    constexpr f32 speed = 5.0f;
    if (player.is_alive()) {
        player.set<world::Velocity>({ direction * speed });
        if (const world::Transform* transform = player.try_get<world::Transform>()) {
            ai::updateChaseAi(world, transform->position); // NPCs seek the player
        }
    }

    world::applyMovement(world, dt);
    world::resolveCollisions(world);

    // Pickups: the player overlapping an item trigger collects it.
    if (player.is_alive()) {
        vector<ecs::Entity> pickedUp;
        world::forEachTriggerOverlap(
            world, [&](ecs::Entity dynamic, ecs::Entity trigger) {
                if (dynamic != player) {
                    return;
                }
                const world::RefId* refId = trigger.try_get<world::RefId>();
                const data::Form* base = refId ? forms.get(refId->base) : nullptr;
                if (!base) {
                    return;
                }
                if (gameplay::Inventory* inventory =
                        player.try_get_mut<gameplay::Inventory>()) {
                    gameplay::addItem(*inventory, base->id);
                }
                pickedUp.push_back(trigger);
            });
        for (ecs::Entity item : pickedUp) {
            if (item.is_alive()) {
                item.destruct();
            }
        }
    }
}

void GameplayScene::drawUi() {
    ImGui::Begin("Player");
    ImGui::TextWrapped("Move with WASD / arrows. Walk over a sword to pick it "
                       "up; the dummy gives chase.");
    if (player.is_alive()) {
        if (const gameplay::Inventory* inventory =
                player.try_get<gameplay::Inventory>()) {
            ImGui::SeparatorText("Inventory");
            if (inventory->items.empty()) {
                ImGui::TextDisabled("(empty)");
            }
            int row = 0;
            for (const gameplay::ItemStack& stack : inventory->items) {
                const data::Form* form = forms.find(stack.item);
                const char* name = form && !form->editorId.empty()
                                       ? form->editorId.c_str()
                                       : "item";
                ImGui::PushID(row++);
                ImGui::Text("%s x%d", name, stack.count);
                ImGui::SameLine();
                if (ImGui::SmallButton("Equip")) {
                    if (gameplay::Equipment* equipment =
                            player.try_get_mut<gameplay::Equipment>()) {
                        gameplay::equip(*equipment, stack.item);
                    }
                }
                ImGui::PopID();
            }
            if (const gameplay::Equipment* equipment =
                    player.try_get<gameplay::Equipment>();
                equipment && equipment->weapon.isValid()) {
                const data::Form* weapon = forms.find(equipment->weapon);
                ImGui::Text("Equipped: %s",
                            weapon && !weapon->editorId.empty()
                                ? weapon->editorId.c_str()
                                : "?");
            }
        }
    }
    ImGui::End();
}

} // namespace game
