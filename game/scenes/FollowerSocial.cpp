// Follower SOCIAL: ambient comments and banter, affinity, the
// recruit preview, perk teaching, the forge upgrade, mercenary
// contracts, death and burial. Split from FollowerController.cpp
// (one TU per trade).
#include "game/scenes/FollowerController.hpp"

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // data::ActorForm, ConsumableForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp" // collectChildren (affinity rules)
#include "data/forms/LocForms.hpp"  // data::TextTable (toasts)
#include "engine/core/Log.hpp"
#include "engine/ui/UiSystem.hpp" // the recruit-preview model
#include "game/ScreenStack.hpp"   // show("recruit")
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/SaveGame.hpp"                        // PendingSaveLayer
#include "game/scenes/NpcDirector.hpp"
#include "game/scenes/NpcMovement.hpp" // groundAt
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp" // applyEffect (revive)
#include "gameplay/actors/ActorState.hpp" // gameplay::FollowerState
#include "gameplay/actors/FollowerForms.hpp" // FollowerClassForm,
                                             // AffinityRuleForm
#include "gameplay/actors/Followers.hpp"  // followTuning, adoptOnHit,
                                          // resolveBleedout,
                                          // affinityDelta
#include "gameplay/combat/Combat.hpp"     // updateLifeState (revive)
#include "gameplay/event/EventBus.hpp"    // gameplay::Event (aggro)
#include "gameplay/interaction/FurnitureForms.hpp" // the grave form
#include "gameplay/inventory/Inventory.hpp" // the player's bag (revive)
#include "gameplay/stats/CoreAttributes.hpp" // the 9 bases (level-ups)
#include "gameplay/stats/Damage.hpp"        // CombatState, updateDowned
#include "gameplay/stats/GameClock.hpp"     // convalescence stamps
#include "gameplay/stats/Injuries.hpp"      // Injuries
#include "world/scene/Components.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/WorldForms.hpp" // world::ReferenceForm

namespace game {


void FollowerController::onAmbientEvent(const FollowerContext& ctx,
                                        const gameplay::Event& event) {
    // Never in sneak (docs/FOLLOWERS.md §6.1) — one check for the sweep.
    if (playerSneaking(ctx)) {
        return;
    }
    const f64 nowHours = ctx.gameClock.gameHours();
    for (const auto& npcPtr : ctx.npcDirector.npcs()) {
        const Npc& npc = *npcPtr;
        if (npc.dead || npc.downed || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>() ||
            !npc.entity.get<gameplay::FollowerState>().followerActive) {
            continue;
        }
        const data::ActorForm* actor = followerActorForm(ctx, npc.entity);
        if (!actor) {
            continue;
        }
        // His ActorForm's comments (the childrenOf pattern, like the
        // affinity rules); most actors have none — the early exit keeps
        // the generic-subscription sweep cheap.
        const vector<const gameplay::CommentForm*> comments =
            data::collectChildren<gameplay::CommentForm>(ctx.forms,
                                                         actor->id);
        for (const gameplay::CommentForm* comment : comments) {
            if (!gameplay::commentMatches(*comment, event.kind, event.tag,
                                          event.value,
                                          event.source == ctx.playerEntity,
                                          ctx.gameTags)) {
                continue;
            }
            const gameplay::CommentClock prerequisite =
                comment->requiresComment.isValid()
                    ? commentClocks_[comment->requiresComment]
                    : gameplay::CommentClock {};
            if (!gameplay::decideComment(*comment, nowHours,
                                         /*sneaking=*/false,
                                         commentClocks_[comment->id],
                                         prerequisite)) {
                continue;
            }
            commentClocks_[comment->id] = { true, nowHours };
            if (ctx.texts) {
                toast(ctx, ctx.texts->format(
                               "follower.says",
                               { std::string_view { actor->displayName },
                                 std::string_view { comment->line } }));
            }
            LOG_INFO("{} comments on {} ('{}')", actor->editorId,
                     comment->event, comment->editorId);
            break; // one line per follower per event — no monologue walls
        }
    }
}

void FollowerController::updateBanter(const FollowerContext& ctx, f32 dt,
                                      f64 nowHours) {
    // The queued REPLY first: interaction.say shows one line at a time,
    // so lineB lands a beat after lineA. [cpp-tuning] the 3 s beat.
    constexpr f32 kBanterReplySeconds = 3.0f;
    if (!pendingReplyLine_.empty()) {
        pendingReplySeconds_ -= dt;
        if (pendingReplySeconds_ <= 0.0f) {
            toast(ctx, std::move(pendingReplyLine_));
            pendingReplyLine_.clear();
        }
    }
    if (lastBanterHours_ < 0.0) {
        lastBanterHours_ = nowHours; // first sweep stamps, never chats
        return;
    }
    if (nowHours - lastBanterHours_ <
        static_cast<f64>(ctx.statsTuning.banterIntervalHours)) {
        return;
    }
    if (playerSneaking(ctx)) {
        return; // the §6.1 sneak rule covers banter too
    }
    // The candidates: ACTIVE, standing, out-of-combat followers.
    struct Chatter {
        const Npc* npc;
        const data::ActorForm* actor;
    };
    vector<Chatter> chatters;
    for (const auto& npcPtr : ctx.npcDirector.npcs()) {
        const Npc& npc = *npcPtr;
        if (npc.dead || npc.downed || npc.weaponDrawn ||
            npc.combatTarget.id() != 0 || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>() ||
            !npc.entity.get<gameplay::FollowerState>().followerActive ||
            !npc.entity.has<world::Transform>()) {
            continue;
        }
        if (const data::ActorForm* actor =
                followerActorForm(ctx, npc.entity)) {
            chatters.push_back({ &npc, actor });
        }
    }
    // The window is due either way: re-stamp so an empty/ineligible party
    // waits a full interval, not a per-frame rescan at the boundary.
    lastBanterHours_ = nowHours;
    if (chatters.size() < 2) {
        return; // banter needs company (docs/FOLLOWERS.md §6.2)
    }
    // The authored bonds (v1: initial values only, no runtime
    // mutation — the doc's evolving matrix comes later).
    vector<const gameplay::FollowerBondForm*> bonds;
    data::forEach<gameplay::FollowerBondForm>(
        ctx.forms, [&](const gameplay::FollowerBondForm& bond) {
            bonds.push_back(&bond);
        });
    // First eligible banter in plugin order (deterministic §8): both of
    // the pair active and NEAR each other, the pure gate passed.
    vector<const gameplay::BanterForm*> banters;
    data::forEach<gameplay::BanterForm>(
        ctx.forms, [&](const gameplay::BanterForm& banter) {
            banters.push_back(&banter);
        });
    for (const gameplay::BanterForm* banterPtr : banters) {
        const gameplay::BanterForm& banter = *banterPtr;
        const Chatter* a = nullptr;
        const Chatter* b = nullptr;
        for (const Chatter& chatter : chatters) {
            if (chatter.actor->id == banter.a) {
                a = &chatter;
            } else if (chatter.actor->id == banter.b) {
                b = &chatter;
            }
        }
        if (!a || !b) {
            continue; // the pair isn't (fully) traveling along
        }
        Vec3 gap = a->npc->entity.get<world::Transform>().position -
                   b->npc->entity.get<world::Transform>().position;
        gap.y = 0.0f;
        if (glm::length(gap) > ctx.statsTuning.banterRangeMeters) {
            continue;
        }
        if (!gameplay::decideBanter(
                banter, gameplay::pairAffinity(bonds, banter.a, banter.b),
                nowHours, commentClocks_[banter.id])) {
            continue;
        }
        commentClocks_[banter.id] = { true, nowHours };
        if (ctx.texts) {
            toast(ctx, ctx.texts->format(
                           "follower.says",
                           { std::string_view { a->actor->displayName },
                             std::string_view { banter.lineA } }));
            pendingReplyLine_ = ctx.texts->format(
                "follower.says",
                { std::string_view { b->actor->displayName },
                  std::string_view { banter.lineB } });
            pendingReplySeconds_ = kBanterReplySeconds;
        }
        LOG_INFO("banter '{}' — {} then {}", banter.editorId,
                 a->actor->editorId, b->actor->editorId);
        return; // one exchange per window
    }
}


void FollowerController::consultFollower(const FollowerContext& ctx,
                                         ecs::Entity follower) {
    if (!follower.is_alive() || !follower.has<gameplay::AbilitySystem>() ||
        !ctx.texts) {
        return;
    }
    const auto& system = follower.get<gameplay::AbilitySystem>();
    const f32 health =
        gameplay::currentValueOf(system, gameplay::attr("health"));
    const f32 maxHealth = glm::max(
        gameplay::currentValueOf(system, gameplay::attr("maxHealth")), 1.0f);
    const i32 pct = static_cast<i32>(
        glm::clamp(100.0f * health / maxHealth, 0.0f, 100.0f));
    u64 injuryCount = 0;
    f32 restHours = 0.0f;
    if (follower.has<gameplay::Injuries>()) {
        const auto& injuries = follower.get<gameplay::Injuries>();
        injuryCount = injuries.list.size();
        restHours = gameplay::convalescenceHours(injuries);
    }
    toast(ctx, ctx.texts->format(
                   "follower.status",
                   { std::string_view {
                         followerDisplayName(ctx, follower) },
                     std::to_string(pct), std::to_string(injuryCount),
                     std::to_string(
                         static_cast<i32>(restHours + 0.5f)) }));
}


// ---- Affinity rules + the recruit preview -------------------------------

void FollowerController::onAffinityEvent(const FollowerContext& ctx,
                                         const gameplay::Event& event,
                                         ecs::Entity dialoguePartner) {
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>()) {
            continue;
        }
        auto& state = npc.entity.get_mut<gameplay::FollowerState>();
        // Eligible: traveling together — or the one being TALKED to (his
        // chat rules must run before recruitment, or affinity-gated
        // recruiting could never unlock).
        if (!state.followerActive && npc.entity != dialoguePartner) {
            continue;
        }
        const data::ActorForm* actor = followerActorForm(ctx, npc.entity);
        if (!actor) {
            continue;
        }
        // His ActorForm's rules (the childrenOf pattern); most actors have
        // none — the sweep stays cheap on this early exit.
        const vector<const gameplay::AffinityRuleForm*> rules =
            data::collectChildren<gameplay::AffinityRuleForm>(ctx.forms,
                                                              actor->id);
        if (rules.empty()) {
            continue;
        }
        gameplay::AffinityEventView view;
        view.kind = event.kind;
        view.tag = event.tag;
        view.sourceIsPlayer = event.source == ctx.playerEntity;
        view.targetIsSelf = event.target == npc.entity;
        const f32 delta = gameplay::affinityDelta(rules, view, ctx.gameTags);
        if (delta == 0.0f) {
            continue;
        }
        const f32 applied = gameplay::addAffinity(state, delta);
        if (applied != 0.0f) {
            LOG_INFO("{} affinity {:+.1f} -> {:.1f}", npc.editorId,
                     applied, state.followerAffinity);
        }
    }
}

void FollowerController::openRecruitPreview(const FollowerContext& ctx,
                                            ecs::Entity follower) {
    if (!ctx.ui || !ctx.screenStack || !ctx.texts ||
        !follower.is_alive() || !follower.has<gameplay::AbilitySystem>() ||
        !follower.has<gameplay::FollowerState>()) {
        return;
    }
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor) {
        return;
    }
    const auto& system = follower.get<gameplay::AbilitySystem>();
    const auto& state = follower.get<gameplay::FollowerState>();
    const auto current = [&](const char* name) {
        return gameplay::currentValueOf(system, gameplay::attr(name));
    };
    const auto whole = [](f32 value) {
        return std::to_string(static_cast<i32>(value + 0.5f));
    };

    ::ui::UiSystem& ui = *ctx.ui;
    ui.setString("recruit", "name", actor->displayName);
    const auto* cls =
        ctx.forms.find<gameplay::FollowerClassForm>(actor->followerClass);
    ui.setString("recruit", "classText",
                 ctx.texts->format("ui.recruit.class",
                                   cls ? cls->displayName : str { "-" }));
    ui.setString("recruit", "levelText",
                 ctx.texts->format("ui.recruit.level",
                                   whole(current("level"))));
    ui.setString("recruit", "affinityText",
                 ctx.texts->format("ui.recruit.affinity",
                                   whole(state.followerAffinity)));
    const auto vital = [&](const char* slot, const char* key,
                           const char* attrName, const char* maxName) {
        ui.setString("recruit", slot,
                     ctx.texts->format(key, { whole(current(attrName)),
                                              whole(current(maxName)) }));
    };
    vital("healthText", "ui.recruit.health", "health", "maxHealth");
    vital("energyText", "ui.recruit.energy", "energy", "maxEnergy");
    vital("essenceText", "ui.recruit.essence", "essence", "maxEssence");

    // The nine attributes (docs/STATS.md §1), one row each — CURRENT
    // values on the partner entity, labels from the loc table.
    static constexpr const char* kAttributes[] = {
        "strength",   "constitution", "grace", "dexterity", "alacrity",
        "perception", "charisma",     "ego",   "insight",
    };
    vector<::ui::UiRow> rows;
    rows.reserve(9);
    for (const char* name : kAttributes) {
        ::ui::UiRow row;
        row.id = name;
        row.c0 = ctx.texts->get(str { "ui.attr." } + name);
        row.c1 = whole(current(name));
        rows.push_back(std::move(row));
    }
    ui.setRows("recruit", std::move(rows));
    ctx.screenStack->show("recruit");
}

// ---- The player learns a perk (the reverse direction) --------------------

void FollowerController::teachPerk(const FollowerContext& ctx,
                                   ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor || !ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AttributeSet>() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    // His teachable perks (TaughtPerkForm children — childrenOf order =
    // plugin order, deterministic §8): the first UNLEARNED one lands.
    const vector<const gameplay::TaughtPerkForm*> perks =
        data::collectChildren<gameplay::TaughtPerkForm>(ctx.forms,
                                                        actor->id);
    auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
    auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    for (const gameplay::TaughtPerkForm* perk : perks) {
        if (gameplay::grantPerk(ctx.forms, perk->ability, perk->effect, set,
                                system,
                                ctx.gameTags) != gameplay::PerkGrant::Granted) {
            continue; // already known (or a skipped data bug): next one
        }
        if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.perkLearned",
                                         perk->displayName));
        }
        LOG_INFO("the player learns '{}' from {}", perk->displayName,
                 actor->editorId);
        return;
    }
    // Nothing new to teach: say so (the option stays visible — v1).
    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.perkNone",
                                     actor->displayName));
    }
}

// ---- The forge upgrade of the base kit -----------------------------------

void FollowerController::forgeUpgrade(const FollowerContext& ctx,
                                      ecs::Entity follower) {
    // [cpp-tuning] the forge price — mirrored by the option's HasItem
    // gate in data (the payFine 40-gold precedent). Data TODO (v1 scope):
    // per-follower obsolescence tiers (level paliers) as child records,
    // and the follower/player-BOTH-at-the-forge check (v1 gates on the
    // player's Zone.Forge tag — the follower walks with him).
    constexpr i32 kForgeCost = 50;
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor || !ctx.playerEntity.is_alive() || !ctx.goldForm ||
        !follower.has<gameplay::Inventory>() ||
        !ctx.playerEntity.has<gameplay::Inventory>()) {
        return;
    }
    auto& items = follower.get_mut<gameplay::Inventory>();
    // The swap list first (§2.2: the upgrade IS the next-tier Form —
    // ArmeDAldric -> ArmeDAldricPlus in data): every unremovable weapon
    // that names a valid upgradesTo tier.
    struct Swap {
        core::Guid from, to;
        i32 count;
    };
    vector<Swap> swaps;
    for (const gameplay::ItemStack& stack : items.items) {
        if (stack.count <= 0) {
            continue;
        }
        const auto* weapon = ctx.forms.find<data::WeaponForm>(stack.item);
        if (weapon && weapon->unremovable && weapon->upgradesTo.isValid() &&
            ctx.forms.find<data::WeaponForm>(weapon->upgradesTo)) {
            swaps.push_back({ stack.item, weapon->upgradesTo, stack.count });
        }
    }
    if (swaps.empty()) {
        // Already at the top tier: refuse WITHOUT charging (why the gold
        // moves here and not through the node's takeItem).
        if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.forgeNothing",
                                         actor->displayName));
        }
        return;
    }
    // Charge (the payFine idiom — the option's HasItem >= 50 gate makes a
    // failure here a modded-data breakage: then do nothing).
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    if (!gameplay::removeItem(bag, ctx.goldForm->id, kForgeCost)) {
        return;
    }
    for (const Swap& swap : swaps) {
        gameplay::removeItem(items, swap.from, swap.count);
        gameplay::addItem(items, swap.to, swap.count);
        if (follower.has<gameplay::Equipment>()) {
            auto& equipment = follower.get_mut<gameplay::Equipment>();
            if (equipment.weapon == swap.from) {
                equipment.weapon = swap.to;
            }
        }
        LOG_INFO("{} forge upgrade {} -> {}", actor->editorId,
                 swap.from.toString(), swap.to.toString());
    }
    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.forgeUpgraded",
                                     actor->displayName));
    }
}

// ---- Mercenaries ----------------------------------------------------------

void FollowerController::hireMercenary(const FollowerContext& ctx,
                                       ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor || !actor->mercenary || !ctx.goldForm ||
        !ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::Inventory>() ||
        !ctx.playerEntity.has<gameplay::AttributeSet>()) {
        return; // not a mercenary (or no live economy): the option is a no-op
    }
    // The REAL price — the pure formula on the player's level and
    // wealth (the option's HasItem gate in data is only the COARSE
    // base-price floor; this is where the scaling bites).
    const f32 playerLevel =
        ctx.playerEntity.get<gameplay::AttributeSet>().level;
    const i32 playerGold = gameplay::itemCount(
        ctx.playerEntity.get<gameplay::Inventory>(), ctx.goldForm->id);
    const i32 price = gameplay::mercenaryPrice(
        actor->contractBasePrice, playerLevel, playerGold, ctx.statsTuning);
    if (playerGold < price) {
        // Short on the SCALED price: refuse, quoting it — this toast IS
        // the price display (v1, stated in the header).
        if (ctx.texts) {
            toast(ctx, ctx.texts->format(
                           "follower.contractShort",
                           { std::string_view { actor->displayName },
                             std::to_string(price) }));
        }
        LOG_INFO("{} refuses — {} gold asked, {} in the bag",
                 actor->editorId, price, playerGold);
        return;
    }
    const bool renewal =
        follower.get<gameplay::FollowerState>().followerActive;
    if (!renewal) {
        // The recruit path unchanged (§2.11) — with its OWN gates
        // (party caps, convalescence) and their toasts. Bounced =
        // no charge (the payFine-idiom reason the gold moves here).
        recruit(ctx, follower);
        if (!follower.get<gameplay::FollowerState>().followerActive) {
            return;
        }
    }
    // Component refs are fetched AFTER recruit(): dropping InCell moved
    // the entity's archetype — an earlier reference would dangle.
    auto& state = follower.get_mut<gameplay::FollowerState>();
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    if (!gameplay::removeItem(bag, ctx.goldForm->id, price)) {
        return; // counted above — only modded data mid-frame gets here
    }
    // The contract stamp: contractDays on top of max(now, current expiry)
    // (gameplay::extendContract) — an early renewal loses no paid hours,
    // a fresh hire (stamp 0) starts from now. Captured so F5/F9 carry it.
    state.followerContractExpiryHours = gameplay::extendContract(
        ctx.gameClock.gameHours(), state.followerContractExpiryHours,
        actor->contractDays);
    warnedContracts_.erase(actor->id); // a new window, a new (one) warning
    ctx.pendingSave.captureEntity(follower, ctx.forms, ctx.gameTags);
    const i32 days = static_cast<i32>(actor->contractDays + 0.5f);
    if (ctx.texts) {
        toast(ctx, ctx.texts->format(
                       renewal ? "follower.contractRenewed"
                               : "follower.contractHired",
                       { std::string_view { actor->displayName },
                         std::to_string(days), std::to_string(price) }));
    }
    LOG_INFO("{} {} for {} gold — contract ends at {:.1f} h",
             actor->editorId, renewal ? "renewed" : "hired", price,
             state.followerContractExpiryHours);
}

// ---- Death, grave, burial ------------------------------------------------

namespace {

// The grave guid namespace — the prefab-child derivation idiom (§2.11):
// combine(followerReference, kGraveNamespace) is stable forever.
const core::Guid kGraveNamespace =
    *core::Guid::fromString("6a1dc0de-e8e8-4000-8000-6772617665f0");

} // namespace

// The shared burial core: create the persistent grave reference at
// `gravePos` (pending layer + live spawn), move the corpse's inventory
// into it, remove the corpse (the picked-up-item idiom). Returns true
// when the corpse entity was destructed (NPC list refresh needed).
bool FollowerController::buryFollower(const FollowerContext& ctx,
                                      ecs::Entity corpse,
                  const Vec3& gravePos) {
    const data::ActorForm* actor = followerActorForm(ctx, corpse);
    if (!actor || !corpse.has<world::RefId>()) {
        return false;
    }
    const auto* grave =
        data::findByEditorId<gameplay::FurnitureForm>(ctx.forms, "Grave");
    if (!grave) {
        LOG_WARN("no 'Grave' FurnitureForm in the data — burial skipped");
        return false;
    }
    const core::Guid corpseRef = corpse.get<world::RefId>().referenceId;
    if (!corpseRef.isValid()) {
        return false; // identity-less corpse: nothing to disable or derive
    }
    const core::Guid graveRef = FollowerController::graveGuidFor(corpseRef);

    // 1) The persistent record (§2.11: the generalized disableReference
    // materialization). Null cell = the persistent set — the grave
    // streams with nobody and survives every reload like the player.
    ctx.pendingSave.createReference(graveRef, grave->id, core::Guid {},
                                    gravePos);

    // 2) The live spawn — the spawnInitialWorld idiom through the scene's
    // Spawner (parent cell = none). Headless callers may skip it.
    ecs::Entity graveEntity {};
    if (ctx.spawnPersistent) {
        world::ReferenceForm reference;
        reference.id = graveRef;
        reference.baseForm = grave->id;
        reference.position = gravePos;
        graveEntity = ctx.spawnPersistent(reference);
    }

    // 3) The corpse's whole inventory moves into the grave (the container
    // screen later deposits/retrieves through the same Inventory).
    if (graveEntity.is_alive()) {
        if (!graveEntity.has<gameplay::Inventory>()) {
            graveEntity.set<gameplay::Inventory>({});
        }
        if (corpse.has<gameplay::Inventory>()) {
            gameplay::transferAllItems(
                corpse.get_mut<gameplay::Inventory>(),
                graveEntity.get_mut<gameplay::Inventory>());
        }
        // Capture NOW: the grave's creates record + its SavedItemForm
        // children live in the pending layer from this frame on.
        ctx.pendingSave.captureEntity(graveEntity, ctx.forms, ctx.gameTags);
    }

    // 4) The corpse leaves the world the picked-up-item way: enabled =
    // false in the pending layer (his authored record never respawns
    // him), then the entity goes.
    ctx.pendingSave.disableReference(corpseRef, ctx.forms, corpse);
    corpse.destruct();

    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.buried", actor->displayName));
    }
    LOG_INFO("{} buried — grave {} at ({:.1f}, {:.1f}, {:.1f})",
             actor->editorId, graveRef.toString(), gravePos.x, gravePos.y,
             gravePos.z);
    return true;
}

core::Guid FollowerController::graveGuidFor(
    const core::Guid& followerReference) {
    return core::Guid::combine(followerReference, kGraveNamespace);
}

str FollowerController::graveOwnerName(const data::FormDatabase& forms,
                                       const core::Guid& graveReference) {
    // Recompute the derivation over the resolved references — the grave
    // guid IS the link, so no owner field needs persisting. References
    // are few (a hand-authored world); this runs on an [E] press.
    str name;
    data::forEach<world::ReferenceForm>(
        forms, [&](const world::ReferenceForm& reference) {
            if (core::Guid::combine(reference.id, kGraveNamespace) !=
                graveReference) {
                return;
            }
            const data::FormHandle baseHandle =
                forms.handleOf(reference.baseForm);
            const data::Form* base = forms.get(baseHandle);
            const reflect::TypeInfo* type = forms.typeOf(baseHandle);
            if (base && type &&
                type->isA(data::ActorForm::staticTypeInfo().id)) {
                name = static_cast<const data::ActorForm*>(base)->displayName;
            }
        });
    return name;
}

bool FollowerController::buryOnSpot(const FollowerContext& ctx,
                                    ecs::Entity corpse) {
    if (!corpse.is_alive() || !corpse.has<world::Transform>()) {
        return false;
    }
    return buryFollower(ctx, corpse, corpse.get<world::Transform>().position);
}

bool FollowerController::buryByContact(const FollowerContext& ctx,
                                       ecs::Entity partner) {
    // The partner's ActorForm identity — buryContact on the DEAD
    // follower's form points at it (data).
    core::Guid partnerForm;
    if (partner.is_alive() && partner.has<world::RefId>()) {
        if (const data::Form* base =
                ctx.forms.get(partner.get<world::RefId>().base)) {
            partnerForm = base->id;
        }
    }
    if (partnerForm.isValid()) {
        for (const auto& npcPtr : ctx.npcDirector.npcs()) {
            const Npc& npc = *npcPtr;
            if (!npc.dead || !npc.entity.is_alive()) {
                continue;
            }
            const data::ActorForm* actor = followerActorForm(ctx, npc.entity);
            if (!actor || actor->buryContact != partnerForm) {
                continue;
            }
            // The authored grave spot (buryMarker reference), grounded on
            // the terrain like every NPC build; fallback: where he lies.
            Vec3 gravePos = npc.entity.get<world::Transform>().position;
            if (const auto* marker = ctx.forms.find<world::ReferenceForm>(
                    actor->buryMarker)) {
                gravePos = marker->position;
                groundAt(ctx.terrainParams, ctx.interiorMode, ctx.physics,
                         gravePos);
            }
            return buryFollower(ctx, npc.entity, gravePos);
        }
    }
    // Nobody to bury: the option stays visible (v1 — no "follower X is
    // dead" condition kind), so the handler answers.
    if (ctx.texts) {
        toast(ctx, ctx.texts->get("follower.buryNone"));
    }
    return false;
}
} // namespace game
