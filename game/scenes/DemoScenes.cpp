#include "game/scenes/DemoScenes.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

#include <imgui.h>

#include "engine/platform/Input.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/condition/Condition.hpp"
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

// Narrative content (defined in base.toml).
const core::Guid kGuardDialogue =
    *core::Guid::fromString("7d000000-0000-4000-8000-000000000001");
const core::Guid kSlayQuest =
    *core::Guid::fromString("7e000000-0000-4000-8000-000000000001");
const core::Guid kSlayTask =
    *core::Guid::fromString("7e000000-0000-4000-8000-0000000000d1");
const core::Guid kReportState =
    *core::Guid::fromString("7e000000-0000-4000-8000-00000000000c");
const core::Guid kCopper =
    *core::Guid::fromString("7f000000-0000-4000-8000-000000000001");

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

// --- NarrativeScene ---------------------------------------------------------

void NarrativeScene::onEnter() {
    WorldDemoScene::onEnter();
    tags.registerTag("Status.Brave"); // gates the brag option
    tags.registerTag("Quest.Active");  // mirrors quest state for dialogue gating
    tags.registerTag("Quest.Ready");
    dialogue.emplace(forms, bus);

    // Accepting the guard's offer begins the quest; the bandit-death and
    // report-to-guard events advance it (the second is fired by a dialogue
    // option, the first by the debug button).
    bus.subscribe(gameplay::eventKind("OnAcceptQuest"),
                  [this](const gameplay::Event&) {
                      quest::beginQuest(questLog, forms, kSlayQuest);
                  });
    const auto advance = [this](const gameplay::Event& event) {
        quest::onQuestEvent(questLog, forms, event, tags);
    };
    bus.subscribe(gameplay::eventKind("OnBanditDeath"), advance);
    bus.subscribe(gameplay::eventKind("OnReportToGuard"), advance);
}

void NarrativeScene::drawUi() {
    // Mirror quest state into player tags so dialogue options can gate on it
    // (the "Accept" option hides once active; "Report" appears once ready).
    const bool questActive = quest::isActive(questLog, kSlayQuest);
    const bool questReady =
        questActive && quest::questState(questLog, kSlayQuest) == kReportState;
    const auto syncTag = [&](const char* name, bool want) {
        const auto tag = tags.find(name);
        if (!tag) {
            return;
        }
        const bool have = playerAbilities.tags.has(*tag);
        if (want && !have) {
            playerAbilities.tags.add(*tag, tags);
        } else if (!want && have) {
            playerAbilities.tags.remove(*tag, tags);
        }
    };
    syncTag("Quest.Active", questActive);
    syncTag("Quest.Ready", questReady);

    // Reward on completion (once).
    if (!rewarded && quest::questStatus(questLog, kSlayQuest) ==
                         quest::QuestStatus::Succeeded) {
        gameplay::addItem(playerInventory, kCopper, 200);
        rewarded = true;
    }

    gameplay::EvalContext ctx;
    ctx.abilitySystem = &playerAbilities;
    ctx.tags = &tags;

    ImGui::Begin("Narrative");

    if (ImGui::Checkbox("Player is Brave (Status.Brave)", &brave)) {
        if (const auto tag = tags.find("Status.Brave")) {
            if (brave) {
                playerAbilities.tags.add(*tag, tags);
            } else {
                playerAbilities.tags.remove(*tag, tags);
            }
        }
    }
    ImGui::Separator();

    if (!dialogue->active()) {
        if (ImGui::Button("Talk to the Guard")) {
            dialogue->start(kGuardDialogue);
        }
    } else if (const quest::DialogueNodeForm* line = dialogue->currentLine()) {
        ImGui::TextWrapped("%s: %s", line->speaker.c_str(), line->text.c_str());
        ImGui::Spacing();
        const auto options = dialogue->options(ctx);
        for (const quest::DialogueNodeForm* option : options) {
            if (ImGui::Button(option->text.c_str())) {
                dialogue->select(*option);
                break; // options just changed
            }
        }
        if (options.empty() && ImGui::Button("(End conversation)")) {
            dialogue->end();
        }
    }

    ImGui::Separator();
    ImGui::Text("Journal:");
    const quest::QuestStatus status = quest::questStatus(questLog, kSlayQuest);
    if (status == quest::QuestStatus::Succeeded) {
        ImGui::BulletText("Slay the Bandits - COMPLETE");
    } else if (questReady) {
        ImGui::BulletText("Slay the Bandits - return to the guard");
    } else if (questActive) {
        ImGui::BulletText("Slay the Bandits - %d / 2 bandits defeated",
                          quest::taskProgress(questLog, kSlayQuest, kSlayTask));
    } else {
        ImGui::TextDisabled("(no active quests - talk to the Guard)");
    }
    ImGui::Text("Copper coins: %d",
                gameplay::itemCount(playerInventory, kCopper));

    ImGui::Separator();
    if (ImGui::Button("Debug: a bandit was defeated")) {
        bus.dispatch({ gameplay::eventKind("OnBanditDeath") });
    }
    ImGui::End();
}

// --- StatsScene -------------------------------------------------------------

gameplay::StatModifiers StatsScene::resonanceModifiers() const {
    gameplay::StatModifiers mods;
    gameplay::Resonance eff = gameplay::effectiveResonance(resonance, survival, tuning);
    eff.onyx += gameplay::injuryResonance(injuries); // N2: injuries → health resonance
    const gameplay::Resonance ar = gameplay::afflictionResonance(afflictions, afflictionDb);
    eff.amber += ar.amber;   // N3: diseases → energy resonance
    eff.garnet += ar.garnet; // N3: psychoses → essence resonance
    gameplay::buildResonanceModifiers(eff, mods);
    if (armorEquipped) {
        gameplay::armorModifiers(sampleArmor, mods); // F3: equipment → derived stats
    }
    gameplay::injuryStatModifiers(injuries, mods);                  // N2 maluses
    gameplay::afflictionStatModifiers(afflictions, afflictionDb, mods); // N3 maluses
    return mods;
}

void StatsScene::seedResources() {
    const gameplay::StatModifiers mods = resonanceModifiers();
    gameplay::recomputeStats(core, vitals, system, derived, &mods);
    vitals.health = gameplay::currentValueOf(system, gameplay::attr("maxHealth"));
    vitals.energy = gameplay::currentValueOf(system, gameplay::attr("maxEnergy"));
    vitals.essence = gameplay::currentValueOf(system, gameplay::attr("maxEssence"));
    gameplay::recomputeStats(core, vitals, system, derived, &mods);
    combat.posture = gameplay::currentValueOf(system, gameplay::attr("maxPosture"));
}

void StatsScene::onEnter() {
    WorldDemoScene::onEnter();
    tags.registerTag("State.Staggered");
    for (const char* statusTag :
         { "Status.Poisoned", "Status.Bleeding", "Status.Mental", "Status.Diseased",
           "Status.Cursed", "Status.Dying" }) {
        tags.registerTag(statusTag);
    }
    tuning = gameplay::resolveStatsTuning(forms); // §5: data or defaults
    gameplay::registerCoreDerivedStats(derived, tuning);
    seedResources();

    // F3 sample gear.
    sampleWeapon.slashAttack = 30.0f;
    sampleWeapon.scalingAttribute = "strength";
    sampleWeapon.scalingK = 1.5f;
    sampleWeapon.postureDamage = 15.0f;
    sampleArmor.armorSlash = 20.0f;
    sampleArmor.resistFire = 15.0f;

    // N3 sample disease (amber/energy) + psychosis (garnet/essence).
    auto disease = std::make_unique<gameplay::AfflictionForm>();
    disease->id = *core::Guid::fromString("d1000000-0000-4000-8000-0000000000a1");
    disease->displayName = "Fever";
    disease->channel = "amber";
    disease->resonancePenalty = -15.0f;
    disease->attributeMalus = "constitution";
    disease->attributeMalusValue = -2.0f;
    disease->recoveryHours = 48.0f;
    sampleDisease = disease->id;
    afflictionDb.add(std::move(disease), gameplay::AfflictionForm::staticTypeInfo());

    auto psychosis = std::make_unique<gameplay::AfflictionForm>();
    psychosis->id = *core::Guid::fromString("d2000000-0000-4000-8000-0000000000a2");
    psychosis->displayName = "Phobia";
    psychosis->channel = "garnet";
    psychosis->resonancePenalty = -20.0f;
    psychosis->attributeMalus = "ego";
    psychosis->attributeMalusValue = -2.0f;
    psychosis->recoveryHours = 72.0f;
    samplePsychosis = psychosis->id;
    afflictionDb.add(std::move(psychosis), gameplay::AfflictionForm::staticTypeInfo());
}

void StatsScene::update(f32 dt) {
    WorldDemoScene::update(dt);
    const f64 gameDt = clock.advance(dt);
    gameplay::tickSurvival(survival, gameDt, tuning);
    gameplay::accrueRest(combat, gameDt); // not in active combat in the demo
    gameplay::updateStagger(combat, system, dt, tags);

    const gameplay::StatModifiers mods = resonanceModifiers();
    gameplay::recomputeStats(core, vitals, system, derived, &mods);
    gameplay::tickBuildup(buildup, system, dt, tags, tuning); // endurance now current

    // Posture regenerates while not staggered.
    if (combat.staggerSeconds <= 0.0f) {
        const f32 maxP = gameplay::currentValueOf(system, gameplay::attr("maxPosture"));
        const f32 regen =
            gameplay::currentValueOf(system, gameplay::attr("postureRegen"));
        combat.posture = std::min(maxP, combat.posture + regen * dt);
    }
}

void StatsScene::drawUi() {
    using namespace gameplay;
    const auto cur = [&](const char* n) { return currentValueOf(system, attr(n)); };

    ImGui::Begin("Character stats (slice)");

    if (ImGui::CollapsingHeader("Attributes (base)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat("strength", &core.strength, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("constitution", &core.constitution, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("grace", &core.grace, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("dexterity", &core.dexterity, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("alacrity", &core.alacrity, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("perception", &core.perception, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("charisma", &core.charisma, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("ego", &core.ego, 0.1f, 0.0f, 40.0f);
        ImGui::DragFloat("insight", &core.insight, 0.1f, 0.0f, 40.0f);
    }

    ImGui::SeparatorText("Vitals");
    const auto bar = [&](const char* label, const char* value, const char* maxField) {
        const f32 v = cur(value);
        const f32 m = cur(maxField);
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f / %.0f", v, m);
        ImGui::ProgressBar(m > 0.0f ? v / m : 0.0f, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::SameLine();
        ImGui::Text("%s", label);
    };
    bar("health", "health", "maxHealth");
    bar("energy", "energy", "maxEnergy");
    bar("essence", "essence", "maxEssence");
    {
        const f32 maxP = cur("maxPosture");
        char overlay[32];
        std::snprintf(overlay, sizeof(overlay), "%.0f / %.0f", combat.posture, maxP);
        ImGui::ProgressBar(maxP > 0.0f ? combat.posture / maxP : 0.0f,
                           ImVec2(-1.0f, 0.0f), overlay);
        ImGui::SameLine();
        ImGui::Text("posture");
    }
    if (const auto st = tags.find("State.Staggered"); st && system.tags.has(*st)) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "STAGGERED");
    }
    if (const auto dead = tags.find("State.Dead"); dead && system.tags.has(*dead)) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "DEAD");
    }

    ImGui::SeparatorText("Resonance (persistent, editable)");
    ImGui::DragFloat("onyx (health)", &resonance.onyx, 0.5f, -100.0f, 200.0f);
    ImGui::DragFloat("amber (energy)", &resonance.amber, 0.5f, -100.0f, 200.0f);
    ImGui::DragFloat("garnet (essence)", &resonance.garnet, 0.5f, -100.0f, 200.0f);
    const Resonance eff = harmonyEffective(effectiveResonance(resonance, survival));
    ImGui::Text("effective (survival + harmony): onyx %.1f  amber %.1f  garnet %.1f",
                eff.onyx, eff.amber, eff.garnet);

    ImGui::SeparatorText("Survival (drag below 75 to drive resonance)");
    ImGui::DragFloat("hunger", &survival.hunger, 0.5f, 0.0f, 100.0f);  // → onyx
    ImGui::DragFloat("thirst", &survival.thirst, 0.5f, 0.0f, 100.0f);  // → onyx
    ImGui::DragFloat("sleep", &survival.sleep, 0.5f, 0.0f, 100.0f);    // → garnet
    ImGui::Text("game time %.1f h   rest %.1f h", clock.gameHours(),
                combat.restSeconds / 3600.0f);

    ImGui::SeparatorText("Derived");
    ImGui::Text("defense %.1f   armor S/B/P %.0f/%.0f/%.0f", cur("defense"),
                cur("armorSlash"), cur("armorBlunt"), cur("armorPierce"));
    ImGui::Text("resist fire/lightning %.0f/%.0f   will %.1f", cur("resistFire"),
                cur("resistLightning"), cur("will"));
    ImGui::Text("maxPosture %.0f   postureRegen %.1f   critSens %.1f",
                cur("maxPosture"), cur("postureRegen"), cur("criticalSensitivity"));

    ImGui::SeparatorText("Actions");
    const StatModifiers mods = resonanceModifiers();
    StatBlock block { core, vitals, system, combat };
    if (ImGui::Button("Slash 50")) {
        applyDamage(block, DamageEvent { { { DamageType::Slash, 50.0f } }, 25.0f },
                    tags, derived, &mods, tuning);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fire 50")) {
        applyDamage(block, DamageEvent { { { DamageType::Fire, 50.0f } }, 0.0f },
                    tags, derived, &mods, tuning);
    }
    ImGui::SameLine();
    if (ImGui::Button("Posture hit")) {
        applyDamage(block, DamageEvent { {}, 40.0f }, tags, derived, &mods, tuning);
    }
    if (ImGui::Button("Wound (-15 onyx)")) {
        resonance.onyx -= 15.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Heal full")) {
        seedResources();
    }
    ImGui::SameLine();
    if (ImGui::Button("Eat / drink")) {
        survival.hunger = 100.0f;
        survival.thirst = 100.0f;
    }
    if (ImGui::Button("Advance 6h (game time)")) {
        const f64 gd = 6.0 * 3600.0;
        clock.gameSeconds += gd;
        tickSurvival(survival, gd, tuning);
    }
    ImGui::SameLine();
    if (ImGui::Button("Sleep 8h (restores sleep, accrues rest, heals injuries)")) {
        gameplay::sleep(clock, survival, combat, 8.0f, tuning);
        gameplay::recoverInjuries(injuries, 8.0f);
        gameplay::recoverAfflictions(afflictions, 8.0f);
    }

    ImGui::SeparatorText("Equipment (F3)");
    ImGui::Checkbox("Equip leather armor (+20 slash armor, +15 fire resist)",
                    &armorEquipped);
    if (ImGui::Button("Attack with equipped weapon (30 slash, ×1.5 strength)")) {
        const DamageResult r = applyDamage(block, weaponDamageEvent(sampleWeapon, system),
                                           tags, derived, &mods, tuning);
        // Roll a cut from the hit — gated by resonance-resistance (no injury at
        // onyx ≥ 0; wound/starve the actor first to see injuries land).
        const f32 maxH = cur("maxHealth");
        const f32 frac = maxH > 0.0f ? r.healthDamage / maxH : 0.0f;
        const f32 onyx = effectiveResonance(resonance, survival, tuning).onyx +
                         injuryResonance(injuries);
        rollInjury(injuries, InjuryType::Cut, BodyPart::Torso,
                   injuryBaseChance(InjuryType::Cut, frac), onyx, rng);
    }

    ImGui::SeparatorText("Status buildup (N1) — fill to endurance to trigger");
    ImGui::Text("poison %.0f / %.0f    bleed %.0f / %.0f", buildup.poison,
                cur("endurancePoison"), buildup.bleed, cur("enduranceBleed"));
    if (ImGui::Button("Poison +40")) {
        addBuildup(buildup, StatusType::Poison, 40.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("Bleed +40")) {
        addBuildup(buildup, StatusType::Bleed, 40.0f);
    }
    ImGui::Text("active statuses:");
    for (const char* statusTag :
         { "Status.Poisoned", "Status.Bleeding", "Status.Mental", "Status.Diseased",
           "Status.Cursed", "Status.Dying" }) {
        if (const auto t = tags.find(statusTag); t && system.tags.has(*t)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.4f, 1.0f), "%s", statusTag);
        }
    }

    ImGui::SeparatorText("Injuries (N2) — heal by sleeping");
    ImGui::Text("movement speed %.0f", cur("movementSpeed")); // drops with leg injury
    static const char* kInjuryTypes[] = { "Bruise", "Cut", "Fracture" };
    static const char* kInjuryParts[] = { "Head", "Torso", "Arms", "Legs" };
    static const char* kInjurySevs[] = { "light", "major", "severe" };
    for (const Injury& inj : injuries.list) {
        ImGui::BulletText("%s %s (%s) — %.0fh left", kInjuryTypes[static_cast<int>(inj.type)],
                          kInjuryParts[static_cast<int>(inj.part)],
                          kInjurySevs[inj.severity], inj.recoveryHoursRemaining);
    }
    if (ImGui::Button("Inflict cut (torso)")) {
        addInjury(injuries, InjuryType::Cut, BodyPart::Torso);
    }
    ImGui::SameLine();
    if (ImGui::Button("Inflict fracture (legs)")) {
        addInjury(injuries, InjuryType::Fracture, BodyPart::Legs);
    }

    ImGui::SeparatorText("Afflictions (N3) — diseases/psychoses, gated by amber/garnet");
    for (const ActiveAffliction& a : afflictions.list) {
        if (const AfflictionForm* def = afflictionDb.find<AfflictionForm>(a.form)) {
            ImGui::BulletText("%s (%s) — %.0fh left", def->displayName.c_str(),
                              def->channel.c_str(), a.recoveryHoursRemaining);
        }
    }
    if (ImGui::Button("Inflict disease (set amber < 0 first)")) {
        if (const AfflictionForm* def = afflictionDb.find<AfflictionForm>(sampleDisease)) {
            const f32 amber = effectiveResonance(resonance, survival, tuning).amber +
                              afflictionResonance(afflictions, afflictionDb).amber;
            inflictAffliction(afflictions, sampleDisease, *def, amber, 1.0, rng);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Inflict psychosis (set garnet < 0 first)")) {
        if (const AfflictionForm* def =
                afflictionDb.find<AfflictionForm>(samplePsychosis)) {
            const f32 garnet = effectiveResonance(resonance, survival, tuning).garnet +
                               afflictionResonance(afflictions, afflictionDb).garnet;
            inflictAffliction(afflictions, samplePsychosis, *def, garnet, 1.0, rng);
        }
    }

    ImGui::End();
}

} // namespace game
