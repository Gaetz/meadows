#include "game/scenes/InteractionController.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp"   // data::ActorForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp"
#include "engine/core/Log.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/platform/Input.hpp"
#include "game/SaveGame.hpp" // PendingSaveLayer
#include "game/scenes/NpcDirector.hpp" // Npc (dead-actor check)
#include "gameplay/actors/ActorState.hpp" // FollowerState (É7 gear access)
#include "gameplay/interaction/FurnitureForms.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/Damage.hpp" // gameplay::CombatState
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/Rest.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"

namespace game {

namespace {

// FOLLOWERS É7: the prompted actor's authored follower identity — any
// actor you COULD recruit exposes his gear ([F]), not just an active
// party member (followerCategory != "" — the É0 identity check, the
// FollowerController::followerActorForm resolution mirrored). Null for
// non-followers and non-actors.
const data::ActorForm* followerActor(const InteractionContext& ctx,
                                     ecs::Entity entity) {
    if (!entity.is_alive() || !entity.has<world::RefId>() ||
        !entity.has<gameplay::FollowerState>()) {
        return nullptr;
    }
    const auto& refId = entity.get<world::RefId>();
    const data::Form* base = ctx.forms.get(refId.base);
    const reflect::TypeInfo* type = ctx.forms.typeOf(refId.base);
    if (!base || !type || !type->isA(data::ActorForm::staticTypeInfo().id)) {
        return nullptr;
    }
    const auto* actor = static_cast<const data::ActorForm*>(base);
    return actor->followerCategory.empty() ? nullptr : actor;
}

// FOLLOWERS É8/É11: the furniture CATEGORY routes special furniture off
// the rest/sleep path — "grave" (homage on [E], the dead follower's
// inventory on [F]) and "mount" (É11 v1: ride it).
bool furnitureCategoryIs(const InteractionContext& ctx,
                         const data::FormHandle& base,
                         std::string_view category) {
    const reflect::TypeInfo* type = ctx.forms.typeOf(base);
    if (!type ||
        !type->isA(gameplay::FurnitureForm::staticTypeInfo().id)) {
        return false;
    }
    const auto* furniture =
        static_cast<const gameplay::FurnitureForm*>(ctx.forms.get(base));
    return furniture && furniture->category == category;
}

} // namespace

// --- B7: doors & worldspace travel ---------------------------------------------------

void InteractionController::update(f32 dt, const InteractionContext& ctx) {
    promptEntity = ecs::Entity {};
    promptKind = PromptKind::None;
    if (talkTimer > 0.0f) {
        talkTimer -= dt;
    }
    if (fadeDirection == 0 && ctx.playMode && ctx.player) {
        // Aim test: nearest interactable within reach, roughly in front
        // of the eye. One scorer for every kind. U4-7: the base reach and
        // the eye height are §5-tunable; the per-kind reach RATIOS stay
        // [cpp-tuning] (they encode relative ergonomics, not feel).
        const f32 reach = ctx.statsTuning.interactionRange;
        const Vec3 eye = ctx.player->position() +
                         Vec3 { 0.0f, ctx.statsTuning.eyeHeight, 0.0f };
        const Vec3 forward = ctx.cameraForward;
        f32 bestScore = 0.55f; // minimum facing alignment
        const auto consider = [&](flecs::entity e, const Vec3& position,
                                  PromptKind kind, f32 reach) {
            const Vec3 to = position + Vec3 { 0.0f, 1.1f, 0.0f } - eye;
            const f32 distance = glm::length(to);
            if (distance > reach || distance < 1e-3f) {
                return;
            }
            const f32 facing = glm::dot(to / distance, forward);
            if (facing > bestScore) {
                bestScore = facing;
                promptEntity = ecs::Entity { e };
                promptKind = kind;
            }
        };
        ctx.doorQuery.each([&](flecs::entity e,
                               const world::Transform& transform,
                               const world::DoorTarget&) {
            consider(e, transform.position, PromptKind::Door, reach);
        });
        ctx.interactQuery.each([&](flecs::entity e,
                                   const world::Transform& transform,
                                   const world::RefId& refId) {
                const ecs::Entity entity { e };
                if (entity.has<world::ItemMarker>()) {
                    consider(e, transform.position, PromptKind::Item,
                             reach * (2.4f / 3.0f));
                } else if (entity.has<world::ActorMarker>() &&
                           entity != ctx.playerEntity) {
                    // Chantier 4 B3: a dead actor is searched, not talked
                    // to. É3: a DOWNED one (follower at 0 HP) is healed.
                    bool isDead = false;
                    bool isDowned = false;
                    for (const auto& npc : ctx.npcs) {
                        if (npc->entity == entity) {
                            isDead = npc->dead;
                            isDowned = npc->downed;
                            break;
                        }
                    }
                    consider(e, transform.position,
                             isDowned ? PromptKind::DownedAlly
                             : isDead ? PromptKind::Corpse
                                      : PromptKind::Actor,
                             reach * (2.8f / 3.0f));
                } else if (entity.has<world::FurnitureMarker>()) {
                    // É8: a grave prompts homage, É11: a mount prompts
                    // the ride — neither ever the rest path.
                    consider(e, transform.position,
                             furnitureCategoryIs(ctx, refId.base, "grave")
                                 ? PromptKind::Grave
                             : furnitureCategoryIs(ctx, refId.base,
                                                   "mount")
                                 ? PromptKind::Mount
                                 : PromptKind::Furniture,
                             reach * (2.4f / 3.0f));
                }
            });

        // The prompt label from the base form's displayName (reflection).
        promptLabel_.clear();
        if (promptEntity.is_alive()) {
            str name;
            const auto& ref = promptEntity.get<world::RefId>();
            if (const data::Form* base = ctx.forms.get(ref.base)) {
                if (const reflect::TypeInfo* type =
                        ctx.forms.typeOf(ref.base)) {
                    if (const reflect::FieldInfo* field =
                            type->findField("displayName");
                        field && field->kind == reflect::FieldKind::Str) {
                        name = std::get<str>(field->get(base));
                    }
                }
            }
            // U4-11: per-kind template ("[E] Prendre {}") + generic-name
            // fallback, both LocStringForm data — languages and mods
            // retune every label without a recompile.
            const auto label = [&](const char* tpl, const char* generic) {
                return ctx.texts.format(
                    tpl, name.empty() ? ctx.texts.get(generic) : name);
            };
            switch (promptKind) {
            case PromptKind::Door:
                promptLabel_ = label("prompt.door", "prompt.door.name");
                break;
            case PromptKind::Item:
                promptLabel_ = label("prompt.take", "prompt.take.name");
                break;
            case PromptKind::Actor:
                promptLabel_ = label("prompt.talk", "prompt.talk.name");
                // É7: a LIVING follower actor also offers his gear on the
                // alternate action — the hint rides the same label (least
                // invasive: one suffix key, loc'd like the rest).
                if (followerActor(ctx, promptEntity)) {
                    promptLabel_ += ctx.texts.get("prompt.gear");
                }
                break;
            case PromptKind::Corpse:
                promptLabel_ = label("prompt.search", "prompt.search.name");
                // É8: a dead FOLLOWER's corpse offers the burial on the
                // alternate action (the É7 gear-suffix idiom).
                if (followerActor(ctx, promptEntity)) {
                    promptLabel_ += ctx.texts.get("prompt.bury");
                }
                break;
            case PromptKind::Furniture:
                promptLabel_ = label("prompt.use", "prompt.use.name");
                break;
            case PromptKind::DownedAlly: // É3: "[E] Soigner {} (potion)"
                promptLabel_ = label("prompt.heal", "prompt.heal.name");
                break;
            case PromptKind::Grave: // É8: "[E] Se recueillir... — [F] Dépôt"
                promptLabel_ = label("prompt.homage", "prompt.homage.name");
                promptLabel_ += ctx.texts.get("prompt.grave");
                break;
            case PromptKind::Mount: // É11: "[E] Monter {}"
                promptLabel_ = label("prompt.mount", "prompt.mount.name");
                break;
            default:
                break;
            }
        }

        if (promptEntity.is_alive() &&
            ctx.actions->pressed(ctx.input, InputAction::Interact)) {
            switch (promptKind) {
            case PromptKind::Door:
                pendingTravel =
                    promptEntity.get<world::DoorTarget>().targetReference;
                fadeDirection = 1;
                break;
            case PromptKind::Item: {
                // Into the inventory; the entity leaves the world and the
                // PENDING layer remembers (chantier 5 B4): the reference
                // stays disabled when its cell reloads, and the disk save
                // flushes enabled = false.
                const auto& ref = promptEntity.get<world::RefId>();
                if (const data::Form* base = ctx.forms.get(ref.base)) {
                    if (!ctx.playerEntity.has<gameplay::Inventory>()) {
                        ctx.playerEntity.set<gameplay::Inventory>({});
                    }
                    gameplay::addItem(
                        ctx.playerEntity.get_mut<gameplay::Inventory>(),
                        base->id, 1);
                    LOG_INFO("Taken: {}", base->editorId);
                }
                if (ref.referenceId.isValid()) {
                    ctx.pendingSave.disableReference(ref.referenceId,
                                                     ctx.forms,
                                                     promptEntity);
                }
                promptEntity.destruct();
                promptEntity = ecs::Entity {};
                break;
            }
            case PromptKind::Actor: {
                // B4: actors with a DialogueForm talk for real; the rest
                // keep the placeholder line.
                const auto& ref = promptEntity.get<world::RefId>();
                const data::Form* base = ctx.forms.get(ref.base);
                const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
                const auto* actor =
                    base && type &&
                            type->isA(data::ActorForm::staticTypeInfo().id)
                        ? static_cast<const data::ActorForm*>(base)
                        : nullptr;
                if (actor && actor->dialogue.isValid()) {
                    // The partner becomes the vendor for B5 barter.
                    ctx.openDialogue(promptEntity, actor->dialogue);
                } else {
                    say(ctx.texts.get("talk.greeting"), 4.0f);
                }
                break;
            }
            case PromptKind::Corpse:
                ctx.openContainer(promptEntity);
                break;
            case PromptKind::DownedAlly: // É3: heal him back up
                if (ctx.reviveAlly) {
                    ctx.reviveAlly(promptEntity);
                }
                break;
            case PromptKind::Grave: // É8: [E] = the homage, NOT the loot
                if (ctx.homage) {
                    ctx.homage(promptEntity);
                }
                break;
            case PromptKind::Mount: // É11: the scene hands over the frame
                if (ctx.mountRide) {
                    ctx.mountRide(promptEntity);
                    promptEntity = ecs::Entity {};
                    promptKind = PromptKind::None;
                }
                break;
            case PromptKind::Furniture: {
                // B7-lite: beds sleep 8h, seats rest 1h — both through the
                // Phase-7 gameplay::sleep() at the black of the fade.
                // Chantier 4 B6: a WORKSTATION opens its UI screen instead
                // (FurnitureForm.screen — crafting tables are furniture +
                // a screen).
                // (É7's [F] gear access lives after this switch — the
                // alternate action, same prompt scan.)
                const auto& ref = promptEntity.get<world::RefId>();
                f32 hours = 1.0f;
                str screen;
                if (const reflect::TypeInfo* type =
                        ctx.forms.typeOf(ref.base);
                    type &&
                    type->isA(gameplay::FurnitureForm::staticTypeInfo().id)) {
                    const auto* furniture =
                        static_cast<const gameplay::FurnitureForm*>(
                            ctx.forms.get(ref.base));
                    if (furniture->category == "bed") { hours = 8.0f; }
                    screen = furniture->screen;
                }
                if (screen.empty() || !ctx.tryShowScreen(screen)) {
                    pendingSleepHours = hours;
                    fadeDirection = 1;
                }
                break;
            }
            default:
                break;
            }
        }

        // FOLLOWERS É7: [F] on a LIVING follower actor opens his
        // gear — THE existing two-panel container screen (transfer +
        // the base-kit/carry guards live in UiRouter). Gated by his
        // opinion: negative affinity = a refusal toast (docs/FOLLOWERS.md
        // §5 — "avis négatif" = followerAffinity < 0, the É4 scale's
        // neutral point).
        if (promptEntity.is_alive() &&
            ctx.actions->pressed(ctx.input, InputAction::InteractAlt)) {
            if (promptKind == PromptKind::Actor) {
                if (const data::ActorForm* actor =
                        followerActor(ctx, promptEntity)) {
                    const auto& state =
                        promptEntity.get<gameplay::FollowerState>();
                    if (state.followerAffinity < 0.0f) {
                        say(ctx.texts.format("follower.gearRefused",
                                             actor->displayName),
                            4.0f);
                    } else if (ctx.openContainer) {
                        ctx.openContainer(promptEntity);
                    }
                }
            } else if (promptKind == PromptKind::Grave) {
                // É8: the special interaction = the dead follower's
                // inventory — THE two-panel container screen, deposits
                // (flowers) and retrievals both ways.
                if (ctx.openContainer) {
                    ctx.openContainer(promptEntity);
                }
            } else if (promptKind == PromptKind::Corpse) {
                // É8: « Enterrer ici » — dead FOLLOWERS only (a bandit
                // stays plain lootable). The closure destructs the
                // corpse: drop the prompt reference right away.
                if (ctx.buryCorpse && followerActor(ctx, promptEntity)) {
                    ctx.buryCorpse(promptEntity);
                    promptEntity = ecs::Entity {};
                    promptKind = PromptKind::None;
                }
            }
        }
    }
    // Fade state machine: out -> travel at black -> in (U4-7: duration
    // §5-tunable, 0.3 s by default).
    const f32 kFadeSpeed =
        1.0f / glm::max(ctx.statsTuning.travelFadeSeconds, 0.01f);
    if (fadeDirection > 0) {
        fadeAlpha_ += dt * kFadeSpeed;
        if (fadeAlpha_ >= 1.0f) {
            fadeAlpha_ = 1.0f;
            if (pendingSleepHours > 0.0f) {
                rest(pendingSleepHours, ctx);
                pendingSleepHours = 0.0f;
            } else {
                ctx.travel(pendingTravel);
                pendingTravel = core::Guid {};
            }
            fadeDirection = -1;
        }
    } else if (fadeDirection < 0) {
        // Hold at black until the world is SOLID under the player: after
        // a travel the arrival cell's meshes may still be decoding and
        // the collider cook is budgeted — fading in before the floor
        // exists dropped the player through it (dev report 2026-07-07).
        // Timeout keeps an authoring hole from freezing the game black.
        if (fadeAlpha_ >= 1.0f && ctx.playMode && ctx.player &&
            ctx.physics) {
            fadeHoldSeconds += dt;
            const phys::RayHit floor = ctx.physics->rayCast(
                ctx.player->position() + Vec3 { 0.0f, 0.5f, 0.0f },
                { 0.0f, -1.0f, 0.0f }, 6.0f);
            if (!floor.hit && fadeHoldSeconds < 5.0f) {
                return; // still cooking — stay black, player stays frozen
            }
        }
        fadeHoldSeconds = 0.0f;
        fadeAlpha_ -= dt * kFadeSpeed;
        if (fadeAlpha_ <= 0.0f) {
            fadeAlpha_ = 0.0f;
            fadeDirection = 0;
        }
    }
}

void InteractionController::rest(f32 hours, const InteractionContext& ctx) {
    if (!ctx.playerEntity.is_alive()) {
        return;
    }
    if (!ctx.playerEntity.has<gameplay::Survival>() ||
        !ctx.playerEntity.has<gameplay::CombatState>()) {
        LOG_WARN("B7: player has no survival stats; rest skipped");
        return;
    }
    gameplay::sleep(ctx.gameClock,
                    ctx.playerEntity.get_mut<gameplay::Survival>(),
                    ctx.playerEntity.get_mut<gameplay::CombatState>(),
                    hours, ctx.statsTuning);
    say(ctx.texts.get(hours >= 8.0f ? "rest.sleep" : "rest.nap"), 3.0f);
    LOG_INFO("B7-lite: rested {} h -> game time {:.2f} h", hours,
             std::fmod(ctx.gameClock.gameHours(), 24.0));
}

void InteractionController::wait(f32 hours, const InteractionContext& ctx) {
    // Waiting passes game time and decays the needs, but restores nothing
    // — that's what beds are for (gameplay::sleep, B7-lite chantier 3).
    const f64 gameDt = static_cast<f64>(hours) * 3600.0;
    ctx.gameClock.gameSeconds += gameDt;
    if (ctx.playerEntity.is_alive()) {
        if (ctx.playerEntity.has<gameplay::Survival>()) {
            gameplay::tickSurvival(
                ctx.playerEntity.get_mut<gameplay::Survival>(), gameDt,
                ctx.statsTuning);
        }
        if (ctx.playerEntity.has<gameplay::CombatState>()) {
            gameplay::accrueRest(
                ctx.playerEntity.get_mut<gameplay::CombatState>(), gameDt);
        }
    }
    LOG_INFO("B6: waited {} h -> game time {:.2f} h", hours,
             std::fmod(ctx.gameClock.gameHours(), 24.0));
}

void InteractionController::beginTravel(const core::Guid& targetReference) {
    pendingTravel = targetReference;
    fadeDirection = 1;
}

void InteractionController::say(str line, f32 seconds) {
    talkLine_ = std::move(line);
    talkTimer = seconds;
}

void InteractionController::reset() {
    promptEntity = ecs::Entity {};
    promptKind = PromptKind::None;
    promptLabel_.clear();
    talkLine_.clear();
    talkTimer = 0.0f;
    pendingTravel = core::Guid {};
    pendingSleepHours = 0.0f;
    fadeAlpha_ = 0.0f;
    fadeDirection = 0;
    fadeHoldSeconds = 0.0f;
}

} // namespace game
