#include <doctest/doctest.h>

#include <algorithm> // std::find (É6 granted-abilities round-trip)

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Rng.hpp"
#include "engine/ecs/World.hpp"
#include "game/SaveGame.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp" // grantAbility, form types (É6)
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/FollowerForms.hpp" // AffinityRuleForm (É4)
#include "gameplay/actors/Followers.hpp"
#include "gameplay/event/EventBus.hpp" // eventKind (É4 rule matching)
#include "gameplay/combat/Combat.hpp"       // updateLifeState (É3 routing)
#include "gameplay/condition/Condition.hpp" // the recruit-refusal gate (É3)
#include "gameplay/save/SaveForms.hpp"
#include "gameplay/stats/CharacterStats.hpp" // recomputeStats
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"   // CombatState, updateDowned (É3)
#include "gameplay/stats/GameTime.hpp" // tickGameTime (the regen gate, É3)
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"
#include "quest/Dialogue.hpp" // DialogueRunner (É4 refusal picking)
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

// FOLLOWERS É1 (docs/CHANTIER-FOLLOWERS.md): the pure follow decision and
// the recruit/dismiss persistence contract at the pending-layer level —
// recruit = ReferenceForm.cell -> 0 as a field-level patch (chantier 5),
// dismiss = cell -> home; the flush is the disk save's input.

using core::Guid;

TEST_CASE("followers: decideFollow bands and boundaries") {
    const gameplay::FollowTuning tuning; // 3.5 / 8 / 1.25 / 40
    const Vec3 player { 0.0f, 0.0f, 0.0f };
    const auto at = [&](f32 distance) {
        return gameplay::decideFollow(Vec3 { distance, 0.0f, 0.0f }, player,
                                      tuning);
    };

    // Near: idle (inclusive at the boundary).
    CHECK_FALSE(at(0.0f).move);
    CHECK_FALSE(at(3.5f).move);
    CHECK_FALSE(at(3.5f).teleport);

    // Walk band: just past near up to catchup, speedScale 1.
    CHECK(at(3.6f).move);
    CHECK(at(3.6f).speedScale == doctest::Approx(1.0f));
    CHECK(at(8.0f).move);
    CHECK(at(8.0f).speedScale == doctest::Approx(1.0f));

    // Catchup band: past catchup up to teleport, catchupSpeed.
    CHECK(at(8.1f).move);
    CHECK(at(8.1f).speedScale == doctest::Approx(1.25f));
    CHECK(at(40.0f).move);
    CHECK(at(40.0f).speedScale == doctest::Approx(1.25f));
    CHECK_FALSE(at(40.0f).teleport);

    // Lost: beyond teleport.
    CHECK(at(40.1f).teleport);
    CHECK_FALSE(at(40.1f).move);

    // The target is always the player; the Y gap never counts (both
    // actors are terrain-grounded).
    const auto uphill = gameplay::decideFollow(
        Vec3 { 2.0f, 30.0f, 0.0f }, player, tuning);
    CHECK_FALSE(uphill.move);
    CHECK(uphill.target.x == doctest::Approx(player.x));
}

TEST_CASE("followers: followTuning mirrors the StatsTuningForm knobs") {
    gameplay::StatsTuningForm form;
    form.followNearRadius = 2.0f;
    form.followCatchupRadius = 5.0f;
    form.followCatchupSpeed = 1.5f;
    form.followTeleportRadius = 25.0f;
    const gameplay::FollowTuning tuning = gameplay::followTuning(form);
    CHECK(tuning.nearRadius == doctest::Approx(2.0f));
    CHECK(tuning.catchupRadius == doctest::Approx(5.0f));
    CHECK(tuning.catchupSpeed == doctest::Approx(1.5f));
    CHECK(tuning.teleportRadius == doctest::Approx(25.0f));
}

namespace {

const Guid kCell = *Guid::fromString("22220000-0000-4000-8000-0000000000f0");
const Guid kFollowerForm =
    *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000f1");
const Guid kFollowerRef =
    *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000f2");

constexpr const char* kFollowerPlugin = R"toml(
[plugin]
id = "11111111-1111-4111-8111-1111111111f0"
name = "base"

[[records]]
form = "11110000-0000-4000-8000-0000000000f0"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Overworld"

[[records]]
form = "22220000-0000-4000-8000-0000000000f0"
type = "CellForm"
new = true
[records.fields]
worldspace = "11110000-0000-4000-8000-0000000000f0"

[[records]]
form = "6a1dc0de-0000-4000-8000-0000000000f1"
type = "ActorForm"
new = true
[records.fields]
editorId = "Aldric"
followerCategory = "major"

[[records]]
form = "6a1dc0de-0000-4000-8000-0000000000f2"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "6a1dc0de-0000-4000-8000-0000000000f1"
cell = "22220000-0000-4000-8000-0000000000f0"
position = [4.0, 0.0, 4.0]
)toml";

ecs::Entity findByRef(ecs::World& world, const Guid& refGuid) {
    ecs::Entity found {};
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity e, const world::RefId& ref) {
            if (ref.referenceId == refGuid) {
                found = ecs::Entity { e };
            }
        });
    return found;
}

u32 countByRef(ecs::World& world, const Guid& refGuid) {
    u32 count = 0;
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity, const world::RefId& ref) {
            if (ref.referenceId == refGuid) {
                ++count;
            }
        });
    return count;
}

// The flush record patching `ref`'s cell field, if any.
std::optional<Guid> flushedCell(const game::PendingSaveLayer& pending,
                                const Guid& ref) {
    const u32 cellFieldId =
        world::ReferenceForm::staticTypeInfo().findField("cell")->id;
    for (const data::Record& record : pending.flush()) {
        if (record.formId != ref) {
            continue;
        }
        if (const auto field = record.fields.find(cellFieldId);
            field != record.fields.end()) {
            return std::get<Guid>(field->second);
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("followers: recruit/dismiss ride the pending layer's cell patch") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerSaveFormTypes(types);
    const auto plugin = data::parsePluginToml(kFollowerPlugin, types, "base");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };

    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    game::PendingSaveLayer pending;
    loader.beforeUnload = [&](data::FormHandle, ecs::Entity cellEntity) {
        pending.captureCell(world, db, cellEntity, tags);
    };
    // The scene's É1 filter: disabled OR re-homed references don't respawn.
    loader.spawnFilter = [&](const Guid& referenceId) {
        return pending.isEnabled(referenceId) &&
               !pending.isRehomed(referenceId);
    };

    const ecs::Entity cellEntity = loader.loadCell(db.handleOf(kCell));
    ecs::Entity follower = findByRef(world, kFollowerRef);
    REQUIRE(follower.is_alive());
    follower.set<gameplay::AttributeSet>({});
    follower.set<gameplay::AbilitySystem>({});
    follower.set<gameplay::FollowerState>({});

    // --- Recruit: the chantier-5 contract, as FollowerController applies
    // it — active state, live cell -> null, InCell dropped, captured.
    follower.get_mut<gameplay::FollowerState>().followerActive = true;
    follower.get_mut<world::RefId>().cell = data::FormHandle {};
    follower.remove<ecs::InCell>(flecs::Wildcard);
    pending.captureEntity(follower, db, tags);

    CHECK(pending.isRehomed(kFollowerRef));
    // The flush (= the disk save's records) carries cell = 0: persistent,
    // exactly like the player.
    const auto recruited = flushedCell(pending, kFollowerRef);
    REQUIRE(recruited.has_value());
    CHECK(*recruited == Guid {});

    // His origin cell unloads (the player walks away): no InCell, so the
    // delete_with sweep misses him — he keeps following.
    loader.unloadCell(db.handleOf(kCell));
    CHECK(findByRef(world, kFollowerRef).is_alive());

    // The cell reloads (the player walks back): the re-home veto keeps
    // the authored record from spawning a DOUBLE of him.
    loader.loadCell(db.handleOf(kCell));
    CHECK(countByRef(world, kFollowerRef) == 1);

    // --- Dismiss (home cell resident): cell -> home (= the authored
    // cell), InCell re-added; the capture drops the cell diff.
    const ecs::Entity homeCell = loader.cellEntity(db.handleOf(kCell));
    REQUIRE(homeCell.is_alive());
    follower.get_mut<gameplay::FollowerState>().followerActive = false;
    follower.get_mut<world::RefId>().cell = db.handleOf(kCell);
    follower.add<ecs::InCell>(homeCell);
    pending.captureEntity(follower, db, tags);

    CHECK_FALSE(pending.isRehomed(kFollowerRef));
    CHECK_FALSE(flushedCell(pending, kFollowerRef).has_value());
    // The saved actor state remembers he is off duty.
    REQUIRE(pending.hasActorState(kFollowerRef));
    CHECK_FALSE(pending.actorState(kFollowerRef).stats->followerActive);

    // Back home, the normal unload/reload cycle owns him again.
    loader.unloadCell(db.handleOf(kCell));
    CHECK_FALSE(findByRef(world, kFollowerRef).is_alive());
    loader.loadCell(db.handleOf(kCell));
    CHECK(countByRef(world, kFollowerRef) == 1);
    (void)cellEntity;
}

// --- É2: the aggro table (gameplay::adoptOnHit — pure, headless) ------------
// Entity ids stand in for the cast: 1 = player, 2/3 = active followers,
// 10/11 = hostiles, 20 = a villager (no roles).

namespace {

constexpr u64 kPlayer = 1;
constexpr u64 kFollower = 2;
constexpr u64 kFollower2 = 3;
constexpr u64 kHostile = 10;
constexpr u64 kHostile2 = 11;

// Roles for `self`, given the cast above.
gameplay::AggroRoles rolesFor(u64 self, u64 source, u64 target,
                              bool selfHasLiveTarget = false,
                              bool friendlyTrial = false) {
    const auto isFollower = [](u64 id) {
        return id == kFollower || id == kFollower2;
    };
    const auto isHostile = [](u64 id) {
        return id == kHostile || id == kHostile2;
    };
    gameplay::AggroRoles roles;
    roles.self = self;
    roles.selfFollower = isFollower(self);
    roles.selfHostile = isHostile(self);
    roles.selfHasLiveTarget = selfHasLiveTarget;
    roles.sourcePlayer = source == kPlayer;
    roles.sourceFollower = isFollower(source);
    roles.sourceHostile = isHostile(source);
    roles.targetPlayer = target == kPlayer;
    roles.targetFollower = isFollower(target);
    roles.targetHostile = isHostile(target);
    roles.friendlyTrial = friendlyTrial;
    return roles;
}

u64 adopt(u64 self, u64 source, u64 target, bool selfHasLiveTarget = false,
          bool friendlyTrial = false) {
    return gameplay::adoptOnHit(
        source, target,
        rolesFor(self, source, target, selfHasLiveTarget, friendlyTrial));
}

} // namespace

TEST_CASE("followers É2: a follower defends the party") {
    // A hostile hits the player -> the idle follower adopts the hostile.
    CHECK(adopt(kFollower, kHostile, kPlayer) == kHostile);
    // A hostile hits a FELLOW follower -> same adoption.
    CHECK(adopt(kFollower, kHostile, kFollower2) == kHostile);
    // Already committed to a live target: no target hopping.
    CHECK(adopt(kFollower, kHostile2, kPlayer, true) == 0);
    // ...unless HE is the one being hit: the victim re-aims.
    CHECK(adopt(kFollower, kHostile2, kFollower, true) == kHostile2);
    // The player's own hits on the party never get adopted (brawl,
    // friendly fire): the source must be a hostile.
    CHECK(adopt(kFollower, kPlayer, kFollower2) == 0);
    // A villager (no follower/hostile role) reacts to nothing.
    CHECK(adopt(20, kHostile, kPlayer) == 0);
}

TEST_CASE("followers É2: the player's initiative is followed") {
    // The player strikes a hostile first -> followers adopt it.
    CHECK(adopt(kFollower, kPlayer, kHostile) == kHostile);
    // But not when committed elsewhere...
    CHECK(adopt(kFollower, kPlayer, kHostile, true) == 0);
    // ...and never a NON-hostile victim (a crime is not an order).
    CHECK(adopt(kFollower, kPlayer, 20) == 0);
}

TEST_CASE("followers É2: hostiles fight followers back") {
    // A follower hits a hostile -> the hostile re-aims at the follower.
    CHECK(adopt(kHostile, kFollower, kHostile) == kFollower);
    // Hit by the PLAYER: keep the default player targeting (no
    // combatTarget — the exact pre-É2 behavior).
    CHECK(adopt(kHostile, kPlayer, kHostile) == 0);
    // Another hostile's brawl is not his problem.
    CHECK(adopt(kHostile, kFollower, kHostile2) == 0);
    // Self-targeting guard: a hostile "hitting itself" adopts nothing.
    CHECK(adopt(kHostile, kHostile, kHostile) == 0);
}

TEST_CASE("followers É2: Combat.FriendlyTrial gates follower adoption") {
    // Every follower adoption path is suppressed...
    CHECK(adopt(kFollower, kHostile, kPlayer, false, true) == 0);
    CHECK(adopt(kFollower, kPlayer, kHostile, false, true) == 0);
    CHECK(adopt(kFollower, kHostile, kFollower, false, true) == 0);
    // ...but hostile retaliation is not (a real enemy stays real).
    CHECK(adopt(kHostile, kFollower, kHostile, false, true) == kFollower);
}

TEST_CASE("followers É2: death disengages whoever targeted the dead") {
    CHECK(gameplay::disengageOnDeath(kHostile, kHostile));
    CHECK_FALSE(gameplay::disengageOnDeath(kHostile, kHostile2));
    CHECK_FALSE(gameplay::disengageOnDeath(kHostile, 0)); // no target: no-op
}

// --- É3: à terre, soin, survie, rotation ------------------------------------
// The sim rules (docs/CHANTIER-FOLLOWERS.md É3): 0 HP under the
// Follower.Protected mirror routes to State.Downed in updateLifeState (the
// ONE life-state write point); the CombatState bleedout clock (the stagger
// timer pattern) resolves through resolveBleedout on the seeded engine RNG.

namespace {

using gameplay::attr;
using gameplay::currentValueOf;

// The TypedDamageTest fixture, plus the É3 vocabulary.
struct DownFixture {
    gameplay::CoreAttributes core;
    gameplay::AttributeSet vitals;
    gameplay::AbilitySystem system;
    gameplay::CombatState combat;
    gameplay::Injuries injuries;
    gameplay::DerivedStatRegistry derived;
    gameplay::GameplayTagRegistry tags;
    gameplay::GameplayTag dead {};
    gameplay::GameplayTag downed {};
    gameplay::GameplayTag shield {};
    gameplay::StatsTuningForm tuning;

    DownFixture() {
        core.strength = core.constitution = core.grace = 20.0f;
        core.dexterity = core.alacrity = core.perception = 20.0f;
        core.charisma = core.ego = core.insight = 20.0f;
        gameplay::registerCoreDerivedStats(derived);
        dead = tags.registerTag("State.Dead");
        downed = tags.registerTag("State.Downed");
        shield = tags.registerTag("Follower.Protected");
        gameplay::registerStatsRuntimeTags(tags); // injury + survival tags
        recompute();
        vitals.health = currentValueOf(system, attr("maxHealth"));
        recompute();
    }
    void recompute() {
        gameplay::recomputeStats(core, vitals, system, derived, nullptr);
    }
    // A lethal blow's terminal write (the §2.9 execution-calc idiom).
    void hitToZero() {
        vitals.health = 0.0f;
        recompute();
        gameplay::updateLifeState(system, tags);
    }
    void protect() { system.tags.add(shield, tags); }
    gameplay::StatBlock block() {
        return gameplay::StatBlock { core, vitals, system, combat };
    }
};

// The first seed whose opening chance(p) draw comes out `wanted`.
u64 seedWithFirstDraw(f64 p, bool wanted) {
    u64 seed = 1;
    while (core::Rng { seed }.chance(p) != wanted) {
        ++seed;
    }
    return seed;
}

} // namespace

TEST_CASE("followers É3: 0 HP routes to Downed under protection, Dead otherwise") {
    // The bandit path is untouched: no protection tag, he just dies.
    DownFixture bandit;
    bandit.hitToZero();
    CHECK(bandit.system.tags.has(bandit.dead));
    CHECK_FALSE(bandit.system.tags.has(bandit.downed));

    // The active follower goes DOWN instead — not dead, revivable.
    DownFixture ally;
    ally.protect();
    ally.hitToZero();
    CHECK(ally.system.tags.has(ally.downed));
    CHECK_FALSE(ally.system.tags.has(ally.dead));

    // A heal above 0 stands him up (the revive path: applyEffect on his
    // stats, then the life-state derive drops the tag).
    ally.vitals.health = 10.0f;
    ally.recompute();
    gameplay::updateLifeState(ally.system, ally.tags);
    CHECK_FALSE(ally.system.tags.has(ally.downed));
    CHECK_FALSE(ally.system.tags.has(ally.dead));
}

TEST_CASE("followers É3: updateDowned counts the bleedout window ONCE") {
    DownFixture f;
    f.protect();
    f.hitToZero();
    REQUIRE(f.system.tags.has(f.downed));
    f.combat.downedSeconds = 2.0f;
    CHECK_FALSE(gameplay::updateDowned(f.combat, f.system, 1.0f, f.tags));
    CHECK(f.combat.downedSeconds == doctest::Approx(1.0f));
    CHECK(gameplay::updateDowned(f.combat, f.system, 1.5f, f.tags));
    CHECK(f.combat.downedSeconds == doctest::Approx(0.0f));
    // No re-fire once resolved, and the tag stays (the RESOLUTION owns it).
    CHECK_FALSE(gameplay::updateDowned(f.combat, f.system, 1.0f, f.tags));
    CHECK(f.system.tags.has(f.downed));

    // Without the tag the clock never ticks (a standing actor).
    DownFixture up;
    up.combat.downedSeconds = 2.0f;
    CHECK_FALSE(gameplay::updateDowned(up.combat, up.system, 1.0f, up.tags));
    CHECK(up.combat.downedSeconds == doctest::Approx(2.0f));
}

TEST_CASE("followers É3: the aggravation roll is gated, seeded, distributed") {
    const gameplay::StatsTuningForm tuning; // death 0.15, worse 0.4
    // Unwounded: no aggravation AND no draw (the stream stays untouched).
    core::Rng gated { 42 };
    const u64 before = gated.rawState();
    CHECK(gameplay::rollAggravation(false, gated, tuning) ==
          gameplay::Aggravation::None);
    CHECK(gated.rawState() == before);

    // Determinism (§8): the same seed replays the same outcomes.
    core::Rng a { 7 };
    core::Rng b { 7 };
    for (int i = 0; i < 64; ++i) {
        CHECK(gameplay::rollAggravation(true, a, tuning) ==
              gameplay::rollAggravation(true, b, tuning));
    }

    // Distribution: death ~15%, worse ~(0.85 x 0.4) = 34% of 10000 draws.
    core::Rng rng { 1234 };
    int deaths = 0;
    int worse = 0;
    for (int i = 0; i < 10000; ++i) {
        switch (gameplay::rollAggravation(true, rng, tuning)) {
        case gameplay::Aggravation::Death:       ++deaths; break;
        case gameplay::Aggravation::WorseInjury: ++worse; break;
        case gameplay::Aggravation::None:        break;
        }
    }
    CHECK(deaths > 1200);
    CHECK(deaths < 1800);
    CHECK(worse > 3000);
    CHECK(worse < 3800);
}

TEST_CASE("followers É3: bleedout recovery — 1 HP, a fresh wound, back up") {
    DownFixture f;
    f.protect();
    f.hitToZero();
    // A seed whose opening death roll (chance downedDeathChance) fails.
    core::Rng rng { seedWithFirstDraw(f.tuning.downedDeathChance, false) };
    gameplay::StatBlock block = f.block();
    const gameplay::BleedoutResult result = gameplay::resolveBleedout(
        block, f.injuries, rng, f.tags, f.tuning);
    CHECK(result.outcome == gameplay::BleedoutOutcome::Recovered);
    CHECK_FALSE(result.aggravated); // no prior wound: no aggravation draw
    CHECK(f.vitals.health == doctest::Approx(1.0f));
    CHECK_FALSE(f.system.tags.has(f.downed));
    CHECK_FALSE(f.system.tags.has(f.dead));
    REQUIRE(f.injuries.list.size() == 1); // the down always costs a wound
    CHECK(f.injuries.list[0].severity == 0);
    // A light first wound does not force convalescence — the rotation
    // milestone needs a SECOND fall for that.
    CHECK_FALSE(gameplay::needsConvalescence(f.injuries));
}

TEST_CASE("followers É3: bleedout death — the roll kills through the normal path") {
    DownFixture f;
    f.protect();
    f.hitToZero();
    core::Rng rng { seedWithFirstDraw(f.tuning.downedDeathChance, true) };
    gameplay::StatBlock block = f.block();
    const gameplay::BleedoutResult result = gameplay::resolveBleedout(
        block, f.injuries, rng, f.tags, f.tuning);
    CHECK(result.outcome == gameplay::BleedoutOutcome::Died);
    CHECK(f.system.tags.has(f.dead));         // the normal OnDeath flow
    CHECK_FALSE(f.system.tags.has(f.downed)); // a corpse is not downed
    CHECK_FALSE(f.system.tags.has(f.shield)); // protection lifted
}

TEST_CASE("followers É3: a wound on a wounded body can aggravate") {
    DownFixture f;
    f.protect();
    f.hitToZero();
    gameplay::addInjury(f.injuries, gameplay::InjuryType::Cut,
                        gameplay::BodyPart::Torso); // already wounded
    // Seed: death roll fails, aggravation-death fails, worse-injury hits.
    u64 seed = 1;
    for (;; ++seed) {
        core::Rng probe { seed };
        if (!probe.chance(f.tuning.downedDeathChance) &&
            !probe.chance(f.tuning.aggravationDeathChance) &&
            probe.chance(f.tuning.aggravationWorseChance)) {
            break;
        }
    }
    core::Rng rng { seed };
    gameplay::StatBlock block = f.block();
    const gameplay::BleedoutResult result = gameplay::resolveBleedout(
        block, f.injuries, rng, f.tags, f.tuning);
    CHECK(result.outcome == gameplay::BleedoutOutcome::Recovered);
    CHECK(result.aggravated);
    REQUIRE(f.injuries.list.size() == 1);   // same type+part stacks severity
    CHECK(f.injuries.list[0].severity == 2); // 0 -> +1 (fresh) -> +1 (worse)
    // Past the severity bar: he demands rest.
    CHECK(gameplay::needsConvalescence(f.injuries));
    CHECK(gameplay::convalescenceHours(f.injuries) == doctest::Approx(48.0f));
}

TEST_CASE("followers É3: convalescence stamps and the recruit-refusal gate") {
    gameplay::FollowerState state;
    CHECK_FALSE(gameplay::followerConvalescent(state, 0.0)); // never downed
    state.followerDownedRecoveryHours = 100.0f;
    CHECK(gameplay::followerConvalescent(state, 50.0));
    CHECK_FALSE(gameplay::followerConvalescent(state, 100.0)); // healed up

    // The dialogue gate is the É1 mirror-tag + the Phase-4 evaluator, as-is.
    gameplay::GameplayTagRegistry tags;
    const gameplay::GameplayTag tag =
        tags.registerTag("Follower.Convalescent");
    gameplay::AbilitySystem player;
    player.tags.add(tag, tags);
    gameplay::EvalContext context;
    context.abilitySystem = &player;
    context.tags = &tags;
    gameplay::ConditionForm clause;
    clause.kind = "HasTag";
    clause.tag = "Follower.Convalescent";
    CHECK(gameplay::evaluateClause(clause, context)); // the refusal shows
    clause.negate = true;
    CHECK_FALSE(gameplay::evaluateClause(clause, context)); // recruit hides
}

TEST_CASE("followers É3: survival extremes drive resonance, never health") {
    // The 'survival floor' is STRUCTURAL: hunger/thirst feed amber and
    // sleep feeds garnet (energy/essence maxima) — no survival path
    // touches health or onyx, so survival alone cannot kill a follower.
    DownFixture f;
    gameplay::Survival survival;
    survival.hunger = survival.thirst = survival.sleep = 0.0f;
    gameplay::updateSurvivalEffects(survival, f.system, f.vitals, f.tags,
                                    f.tuning);
    for (const gameplay::ActiveEffect& active : f.system.activeEffects) {
        const bool resonanceOnly = active.attribute == attr("amber") ||
                                   active.attribute == attr("garnet");
        CHECK(resonanceOnly);
    }
    f.recompute();
    gameplay::updateLifeState(f.system, f.tags);
    CHECK(f.vitals.health > 0.0f);
    CHECK_FALSE(f.system.tags.has(f.dead));
}

TEST_CASE("followers É3: no health regen while downed") {
    DownFixture f;
    f.protect();
    f.hitToZero();
    REQUIRE(f.system.tags.has(f.downed));
    gameplay::StatusBuildup buildup;
    gameplay::Resonance resonance;
    gameplay::ResonanceDecays decays;
    gameplay::Survival survival;
    gameplay::GameTimeTickArgs args { f.core,    f.vitals,  f.system,
                                      f.combat,  buildup,   survival,
                                      f.injuries, resonance, decays,
                                      f.derived, f.tags,    f.tuning };
    const gameplay::StatModifiers mods;
    gameplay::tickGameTime(args, 3600.0, mods); // one game-hour
    CHECK(f.vitals.health == doctest::Approx(0.0f)); // no silent self-revive
}

// --- É4: recrutement complet + affinité --------------------------------------
// The new evaluator clause (FollowerAffinityAtLeast — one entry in the OCP
// dispatch table, reading the DIALOGUE PARTNER half of EvalContext), the
// AffinityRuleForm matcher (the QuestTaskForm event+filterTag matching) and
// the passive time-together accrual (the VendorState hour-stamp idiom).

TEST_CASE("followers É4: FollowerAffinityAtLeast reads the dialogue partner") {
    gameplay::ConditionForm clause;
    clause.kind = "FollowerAffinityAtLeast";
    clause.value = 10.0f;

    gameplay::EvalContext context; // player-only context: no partner
    CHECK_FALSE(gameplay::evaluateClause(clause, context)); // fails closed

    gameplay::FollowerState partner;
    context.partnerFollower = &partner;
    partner.followerAffinity = 9.9f;
    CHECK_FALSE(gameplay::evaluateClause(clause, context));
    partner.followerAffinity = 10.0f;
    CHECK(gameplay::evaluateClause(clause, context)); // inclusive threshold

    // negate flips the clause — the refusal-option combination.
    clause.negate = true;
    CHECK_FALSE(gameplay::evaluateClause(clause, context));
    partner.followerAffinity = 0.0f;
    CHECK(gameplay::evaluateClause(clause, context));

    // The one-line editor reading (chantier 8.9 contract).
    clause.negate = false;
    CHECK(gameplay::conditionSummary(clause) == "if affinity >= 10");
}

TEST_CASE("followers É4: affinity rules match event, filterTag and parties") {
    gameplay::GameplayTagRegistry tags;
    const gameplay::GameplayTag bandit = tags.registerTag("Faction.Bandits.East");

    gameplay::AffinityRuleForm chat;      // any OnFollowerChat: +5
    chat.event = "OnFollowerChat";
    chat.delta = 5.0f;
    gameplay::AffinityRuleForm friendly;  // player hits ME: -10
    friendly.event = "OnHitTaken";
    friendly.sourcePlayer = true;
    friendly.targetSelf = true;
    friendly.delta = -10.0f;
    gameplay::AffinityRuleForm banditSlain; // a Faction.Bandits* death: +2
    banditSlain.event = "OnDeath";
    banditSlain.filterTag = "Faction.Bandits";
    banditSlain.delta = 2.0f;
    const vector<const gameplay::AffinityRuleForm*> rules {
        &chat, &friendly, &banditSlain
    };

    gameplay::AffinityEventView view;
    view.kind = gameplay::eventKind("OnFollowerChat");
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(5.0f));

    // OnHitTaken needs BOTH party filters.
    view.kind = gameplay::eventKind("OnHitTaken");
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(0.0f));
    view.sourceIsPlayer = true;
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(0.0f));
    view.targetIsSelf = true;
    CHECK(gameplay::affinityDelta(rules, view, tags) ==
          doctest::Approx(-10.0f));

    // filterTag: the event's tag must DESCEND from it (tags.isA — the
    // QuestTaskForm matching); a tagless event never matches a filter.
    view = {};
    view.kind = gameplay::eventKind("OnDeath");
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(0.0f));
    view.tag = bandit; // Faction.Bandits.East isA Faction.Bandits
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(2.0f));
    view.tag = tags.registerTag("Faction.CityGuard");
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(0.0f));

    // Unknown event: nothing moves.
    view = {};
    view.kind = gameplay::eventKind("OnNoise");
    CHECK(gameplay::affinityDelta(rules, view, tags) == doctest::Approx(0.0f));
}

TEST_CASE("followers É4: passive accrual math and the ±100 clamp") {
    gameplay::FollowerState state;
    // The first stamp (delta 0) and clock hiccups are inert.
    CHECK(gameplay::accrueTimeTogether(state, 0.0f, 0.5f) ==
          doctest::Approx(0.0f));
    CHECK(gameplay::accrueTimeTogether(state, -1.0f, 0.5f) ==
          doctest::Approx(0.0f));
    CHECK(state.followerHoursTogether == doctest::Approx(0.0f));

    // 2 game-hours at 0.5/h: hours accumulate, affinity follows.
    CHECK(gameplay::accrueTimeTogether(state, 2.0f, 0.5f) ==
          doctest::Approx(1.0f));
    CHECK(state.followerHoursTogether == doctest::Approx(2.0f));
    CHECK(state.followerAffinity == doctest::Approx(1.0f));

    // The clamp: growth saturates at +100 (hours keep counting).
    state.followerAffinity = 99.5f;
    CHECK(gameplay::accrueTimeTogether(state, 2.0f, 0.5f) ==
          doctest::Approx(0.5f));
    CHECK(state.followerAffinity == doctest::Approx(100.0f));
    CHECK(state.followerHoursTogether == doctest::Approx(4.0f));

    // addAffinity clamps the floor too and reports the APPLIED change.
    CHECK(gameplay::addAffinity(state, -300.0f) == doctest::Approx(-200.0f));
    CHECK(state.followerAffinity == doctest::Approx(-100.0f));
}

namespace {

// The Maela demo (village.toml É4), rebuilt headless: one question, three
// mutually exclusive gatings — recruit (affinity>=10 AND level>=2),
// refusal A (NOT affinity>=10), refusal B (affinity>=10 AND NOT level>=2).
const Guid kMaelaDialogue = *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000e0");
const Guid kMaelaRoot = *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000e1");
const Guid kRecruitOpt = *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000e2");
const Guid kRefusalA = *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000e3");
const Guid kRefusalB = *Guid::fromString("6a1dc0de-0000-4000-8000-0000000000e4");

data::FormDatabase buildRecruitDialogueDb() {
    data::FormDatabase db;
    auto dialogue = std::make_unique<quest::DialogueForm>();
    dialogue->id = kMaelaDialogue;
    dialogue->rootNode = kMaelaRoot;
    db.add(std::move(dialogue), quest::DialogueForm::staticTypeInfo());

    u64 nextGuid = 0xf0;
    const auto addNode = [&](const Guid& id, const Guid& parent,
                             const char* speaker, i32 order) {
        auto node = std::make_unique<quest::DialogueNodeForm>();
        node->id = id;
        node->parent = parent;
        node->speaker = speaker;
        node->order = order;
        db.add(std::move(node), quest::DialogueNodeForm::staticTypeInfo());
    };
    const auto addClause = [&](const Guid& parent, const char* kind,
                               const char* attribute, f32 value,
                               bool negate) {
        auto clause = std::make_unique<gameplay::ConditionForm>();
        clause->id = Guid { 0x6a1dc0de00004000ull,
                            0x8000000000000000ull + nextGuid++ };
        clause->parent = parent;
        clause->kind = kind;
        clause->attribute = attribute;
        clause->value = value;
        clause->negate = negate;
        db.add(std::move(clause), gameplay::ConditionForm::staticTypeInfo());
    };

    addNode(kMaelaRoot, {}, "Maela", 0);
    addNode(kRecruitOpt, kMaelaRoot, "Player", 1);
    addClause(kRecruitOpt, "FollowerAffinityAtLeast", "", 10.0f, false);
    addClause(kRecruitOpt, "AttributeAtLeast", "level", 2.0f, false);
    addNode(kRefusalA, kMaelaRoot, "Player", 2);
    addClause(kRefusalA, "FollowerAffinityAtLeast", "", 10.0f, true);
    addNode(kRefusalB, kMaelaRoot, "Player", 3);
    addClause(kRefusalB, "FollowerAffinityAtLeast", "", 10.0f, false);
    addClause(kRefusalB, "AttributeAtLeast", "level", 2.0f, true);
    return db;
}

} // namespace

TEST_CASE("followers É4: the failing condition picks the refusal option") {
    const data::FormDatabase db = buildRecruitDialogueDb();
    gameplay::EventBus bus;
    gameplay::GameplayTagRegistry tags;

    gameplay::AttributeSet attributes; // level defaults to 1 (the É0 boot)
    gameplay::AbilitySystem player;
    gameplay::initializeCurrent(player, attributes);
    gameplay::FollowerState partner;

    gameplay::EvalContext context;
    context.abilitySystem = &player;
    context.tags = &tags;
    context.partnerFollower = &partner;

    quest::DialogueRunner runner { db, bus };
    REQUIRE(runner.start(kMaelaDialogue));

    const auto only = [&]() -> Guid {
        const auto options = runner.options(context);
        REQUIRE(options.size() == 1); // the gatings are mutually exclusive
        return options[0]->id;
    };

    // Fresh save: affinity 0, level 1 -> the affinity hint (refusal A).
    CHECK(only() == kRefusalA);

    // Affinity earned, level still 1 -> the MORE SPECIFIC level hint (B).
    partner.followerAffinity = 15.0f;
    CHECK(only() == kRefusalB);

    // Both met -> the real recruit option.
    gameplay::setBaseValue(attributes, gameplay::attr("level"), 2.0f);
    gameplay::initializeCurrent(player, attributes);
    CHECK(only() == kRecruitOpt);

    // And back below the affinity bar (a friendly-fire slide): A again.
    partner.followerAffinity = 5.0f;
    CHECK(only() == kRefusalA);
}

// --- É5: classes, niveaux, évolution ------------------------------------------
// The pure rules (docs/CHANTIER-FOLLOWERS.md É5): the age curve applied as
// per-tick StatModifiers (the equipmentMods fold — §2.9: nothing persisted,
// no synthetic effects), the player-linked level sync, the +1 attribute
// point walk, and the curve DELTA on level changes (bonus points survive;
// the recompute path preserves vitals — no mid-game full heal).

TEST_CASE("followers É5: age multipliers — ageless, onset, slope, floor") {
    const gameplay::StatsTuningForm tuning; // onset 45, .008/.005, floor .5
    // Ageless (ActorForm.age default 0) and below onset: both stay 1.
    CHECK(gameplay::ageMultipliers(0.0f, tuning).physical == 1.0f);
    CHECK(gameplay::ageMultipliers(0.0f, tuning).mental == 1.0f);
    CHECK(gameplay::ageMultipliers(28.0f, tuning).physical == 1.0f); // Aldric
    CHECK(gameplay::ageMultipliers(45.0f, tuning).mental == 1.0f);   // the edge
    // Maela (52): seven years past the onset, body ages faster than mind.
    const gameplay::AgeMultipliers maela =
        gameplay::ageMultipliers(52.0f, tuning);
    CHECK(maela.physical == doctest::Approx(1.0f - 7.0f * 0.008f));
    CHECK(maela.mental == doctest::Approx(1.0f - 7.0f * 0.005f));
    CHECK(maela.mental > maela.physical);
    // Methuselah: both clamp at the floor.
    const gameplay::AgeMultipliers ancient =
        gameplay::ageMultipliers(500.0f, tuning);
    CHECK(ancient.physical == doctest::Approx(tuning.ageFloor));
    CHECK(ancient.mental == doctest::Approx(tuning.ageFloor));
}

TEST_CASE("followers É5: age mods shrink CURRENT attributes, never the maxima") {
    const gameplay::StatsTuningForm tuning;
    // The fold contract (armorModifiers): multiply into existing entries.
    gameplay::StatModifiers mods;
    mods.mul[attr("strength")] = 0.5f;
    gameplay::foldAgeModifiers(52.0f, tuning, mods);
    CHECK(mods.mul[attr("strength")] ==
          doctest::Approx(0.5f * (1.0f - 7.0f * 0.008f)));
    CHECK(mods.mul[attr("insight")] ==
          doctest::Approx(1.0f - 7.0f * 0.005f));
    // Ageless: a strict no-op.
    gameplay::StatModifiers none;
    gameplay::foldAgeModifiers(0.0f, tuning, none);
    CHECK(none.mul.empty());
    CHECK(none.add.empty());

    // Through the recompute: current strength drops, but the primary
    // maxima derive from BASE attributes (docs/STATS.md §2) — the doc's
    // « compétence_effective = base × multiplicateur », not a health nerf.
    DownFixture f;
    const f32 maxBefore = currentValueOf(f.system, attr("maxHealth"));
    const f32 strengthBefore = currentValueOf(f.system, attr("strength"));
    gameplay::StatModifiers age;
    gameplay::foldAgeModifiers(52.0f, tuning, age);
    gameplay::recomputeStats(f.core, f.vitals, f.system, f.derived, &age);
    CHECK(currentValueOf(f.system, attr("strength")) <
          strengthBefore - 0.5f);
    CHECK(currentValueOf(f.system, attr("maxHealth")) ==
          doctest::Approx(maxBefore));
}

TEST_CASE("followers É5: level sync — tracking, catch-up, first meet, stamps") {
    using gameplay::syncFollowerLevel;
    // First meeting (lastSyncedFrom 0, the É0 default): stamp only — no
    // retroactive gain from the player's pre-acquaintance levels.
    gameplay::LevelSync s = syncFollowerLevel(1.0f, 0.0f, 4.0f, true, false);
    CHECK(s.level == 1.0f);
    CHECK(s.syncedFrom == 4.0f);
    CHECK(s.pointsGained == 0);
    // Active: 1:1 tracking, one +1 point per level gained together.
    s = syncFollowerLevel(2.0f, 2.0f, 5.0f, true, false);
    CHECK(s.level == 5.0f);
    CHECK(s.syncedFrom == 5.0f);
    CHECK(s.pointsGained == 3);
    // No player movement: nothing moves.
    s = syncFollowerLevel(3.0f, 5.0f, 5.0f, true, false);
    CHECK(s.level == 3.0f);
    CHECK(s.pointsGained == 0);
    // The re-meet: half the gap accrued apart, FLOORED, no points.
    s = syncFollowerLevel(2.0f, 2.0f, 7.0f, false, false); // gap 5 -> +2
    CHECK(s.level == 4.0f);
    CHECK(s.syncedFrom == 7.0f);
    CHECK(s.pointsGained == 0);
    s = syncFollowerLevel(2.0f, 2.0f, 3.0f, false, false); // gap 1 -> +0
    CHECK(s.level == 2.0f);
    // mainCharacter (the story exception): full catch-up, still no points.
    s = syncFollowerLevel(2.0f, 2.0f, 7.0f, false, true);
    CHECK(s.level == 7.0f);
    CHECK(s.pointsGained == 0);
    // Console-lowered player: stamp the new value, gain nothing.
    s = syncFollowerLevel(4.0f, 6.0f, 3.0f, true, false);
    CHECK(s.level == 4.0f);
    CHECK(s.syncedFrom == 3.0f);
    CHECK(s.pointsGained == 0);
}

TEST_CASE("followers É5: the +1 point walks the player's attributes descending") {
    gameplay::CoreAttributes player;   // defaults: 6s, insight 0
    gameplay::CoreAttributes follower; // same
    // Equal everywhere: no attribute is strictly greater -> no point.
    CHECK_FALSE(gameplay::bonusAttribute(player, follower).has_value());
    // The player's best attribute above the follower's takes it.
    player.strength = 10.0f;
    auto pick = gameplay::bonusAttribute(player, follower);
    REQUIRE(pick.has_value());
    CHECK(str { gameplay::kCoreAttributeNames[*pick] } == "strength");
    // The follower already better there: the walk continues DESCENDING —
    // the player's second-best wins.
    follower.strength = 12.0f;
    player.ego = 9.0f;
    player.grace = 8.0f;
    pick = gameplay::bonusAttribute(player, follower);
    REQUIRE(pick.has_value());
    CHECK(str { gameplay::kCoreAttributeNames[*pick] } == "ego");
    // Ties keep the canonical field order (strength before ego).
    gameplay::CoreAttributes tied;
    tied.strength = 8.0f;
    tied.ego = 8.0f;
    gameplay::CoreAttributes fresh;
    pick = gameplay::bonusAttribute(tied, fresh);
    REQUIRE(pick.has_value());
    CHECK(str { gameplay::kCoreAttributeNames[*pick] } == "strength");
    // A follower above the player EVERYWHERE: the implicit no-point case.
    gameplay::CoreAttributes titan;
    for (u32 i = 0; i < gameplay::kCoreAttributeCount; ++i) {
        gameplay::coreAttributeRef(titan, i) = 20.0f;
    }
    CHECK_FALSE(gameplay::bonusAttribute(player, titan).has_value());
}

TEST_CASE("followers É5: curves at spawn, DELTA on level-up — bonuses and vitals survive") {
    gameplay::FollowerClassForm cls; // the ClassWarrior shape
    cls.strengthBase = 7.0f;
    cls.strengthPerLevel = 1.5f;
    cls.constitutionBase = 7.0f;
    cls.constitutionPerLevel = 1.0f;

    DownFixture f;
    // Spawn: the ABSOLUTE write (before initializeActorStats fills vitals).
    gameplay::applyFollowerClass(f.core, cls, 1.0f);
    CHECK(f.core.strength == 7.0f);
    CHECK(f.core.constitution == 7.0f);
    CHECK(f.core.grace == 6.0f); // class defaults = villager defaults
    f.recompute();
    const f32 maxBefore = currentValueOf(f.system, attr("maxHealth"));

    // Mid-game: wounded, and a bonus point earned earlier.
    f.vitals.health = 12.0f;
    f.core.strength += 1.0f; // the É5 +1 (now 8)

    // Level 1 -> 3 applies the curve DELTA — the bonus point SURVIVES
    // (an absolute overwrite would erase it).
    gameplay::applyClassLevelChange(f.core, cls, 1.0f, 3.0f);
    CHECK(f.core.strength == doctest::Approx(8.0f + 2.0f * 1.5f));
    CHECK(f.core.constitution == doctest::Approx(7.0f + 2.0f * 1.0f));
    CHECK(f.core.grace == 6.0f); // flat curves stay put

    // The vitals-preserving recompute (the level-change path): health
    // stays where the fight left it while the maxima grew — never
    // initializeActorStats (which would full-heal, spawn-only).
    f.recompute();
    CHECK(f.vitals.health == doctest::Approx(12.0f));
    CHECK(currentValueOf(f.system, attr("maxHealth")) > maxBefore);
}

// ---- É6: powers, class perks, taught perks -----------------------------------

namespace {

Guid guid(const char* text) { return *Guid::fromString(text); }

// A minimal É6 data plugin: a class whose level-1 perk grants an ability
// and whose level-3 perk applies a tagged infinite effect — plus one
// DISCIPLINE-BREAKING perk effect (no grantedTag) that must be skipped.
constexpr const char* kPerkPlugin = R"([plugin]
id = "60000000-0000-4000-8000-000000000001"
name = "perk-data"

[[records]]
form = "60000000-0000-4000-8000-000000000010"
type = "FollowerClassForm"
new = true
[records.fields]
editorId = "TestWarrior"
combatStyle = "melee"

[[records]]
form = "60000000-0000-4000-8000-000000000011"
type = "EffectForm"
new = true
[records.fields]
editorId = "TestPowerCooldown"
duration = "duration"
durationSeconds = 30.0
grantedTag = "Cooldown.TestPower"

[[records]]
form = "60000000-0000-4000-8000-000000000012"
type = "AbilityForm"
new = true
[records.fields]
editorId = "TestPower"
cooldown = "60000000-0000-4000-8000-000000000011"

[[records]]
form = "60000000-0000-4000-8000-000000000013"
type = "ClassPerkForm"
new = true
[records.fields]
parent = "60000000-0000-4000-8000-000000000010"
level = 1.0
ability = "60000000-0000-4000-8000-000000000012"

[[records]]
form = "60000000-0000-4000-8000-000000000014"
type = "EffectForm"
new = true
[records.fields]
editorId = "TestPerkBuff"
attribute = "energyRegen"
op = "add"
magnitude = 5.0
duration = "infinite"
grantedTag = "Perk.TestBuff"

[[records]]
form = "60000000-0000-4000-8000-000000000015"
type = "ClassPerkForm"
new = true
[records.fields]
parent = "60000000-0000-4000-8000-000000000010"
level = 3.0
effect = "60000000-0000-4000-8000-000000000014"

[[records]]
form = "60000000-0000-4000-8000-000000000016"
type = "EffectForm"
new = true
[records.fields]
editorId = "TestUntaggedBuff"
attribute = "energyRegen"
op = "add"
magnitude = 99.0
duration = "infinite"

[[records]]
form = "60000000-0000-4000-8000-000000000017"
type = "ClassPerkForm"
new = true
[records.fields]
parent = "60000000-0000-4000-8000-000000000010"
level = 1.0
effect = "60000000-0000-4000-8000-000000000016"
)";

data::FormDatabase buildPerkDb(data::FormTypeRegistry& types) {
    gameplay::registerFollowerFormTypes(types);
    gameplay::registerGameplayFormTypes(types); // EffectForm, AbilityForm
    const auto plugin = data::parsePluginToml(kPerkPlugin, types, "perks");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    return db;
}

} // namespace

TEST_CASE("followers É6: syncClassPerks gates by level and never doubles") {
    data::FormTypeRegistry types;
    data::FormDatabase db = buildPerkDb(types);
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("Perk.TestBuff");
    tags.registerTag("Cooldown.TestPower");
    const Guid classGuid = guid("60000000-0000-4000-8000-000000000010");
    const Guid power = guid("60000000-0000-4000-8000-000000000012");

    gameplay::AttributeSet set;
    gameplay::AbilitySystem system;
    gameplay::initializeCurrent(system, set);

    // Level 1: the ability perk lands; the level-3 effect stays locked;
    // the DISCIPLINE-BREAKING untagged effect is skipped (never applied).
    CHECK(gameplay::syncClassPerks(db, classGuid, 1.0f, set, system, tags) ==
          1);
    REQUIRE(system.grantedAbilities.size() == 1);
    CHECK(system.grantedAbilities[0] == power);
    CHECK(system.activeEffects.empty());

    // Re-sync at the same level: fully idempotent.
    CHECK(gameplay::syncClassPerks(db, classGuid, 1.0f, set, system, tags) ==
          0);
    CHECK(system.grantedAbilities.size() == 1);
    CHECK(system.activeEffects.empty());

    // Level 3 (a level-up re-sync): the tagged infinite effect lands ONCE.
    CHECK(gameplay::syncClassPerks(db, classGuid, 3.0f, set, system, tags) ==
          1);
    CHECK(system.grantedAbilities.size() == 1);
    CHECK(system.activeEffects.size() == 1);
    CHECK(system.tags.has(*tags.find("Perk.TestBuff")));

    // Re-sync: the grantedTag dedup blocks the re-stack (§2.9 stays sane).
    CHECK(gameplay::syncClassPerks(db, classGuid, 3.0f, set, system, tags) ==
          0);
    CHECK(system.activeEffects.size() == 1);
}

TEST_CASE("followers É6: grantPerk — the learned-perk dedup (grantedTag)") {
    data::FormTypeRegistry types;
    data::FormDatabase db = buildPerkDb(types);
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("Perk.TestBuff");

    gameplay::AttributeSet set;
    gameplay::AbilitySystem system;
    gameplay::initializeCurrent(system, set);
    const Guid effect = guid("60000000-0000-4000-8000-000000000014");

    // First lesson: learned. Second: already known — nothing stacks.
    CHECK(gameplay::grantPerk(db, Guid {}, effect, set, system, tags) ==
          gameplay::PerkGrant::Granted);
    CHECK(gameplay::grantPerk(db, Guid {}, effect, set, system, tags) ==
          gameplay::PerkGrant::AlreadyKnown);
    CHECK(system.activeEffects.size() == 1);

    // An untagged effect is a data bug: skipped, applied zero times.
    const Guid untagged = guid("60000000-0000-4000-8000-000000000016");
    CHECK(gameplay::grantPerk(db, Guid {}, untagged, set, system, tags) ==
          gameplay::PerkGrant::Skipped);
    CHECK(system.activeEffects.size() == 1);
}

TEST_CASE("followers É6: grantedAbilities round-trip the save layer") {
    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    const Guid ref = guid("60000000-0000-4000-8000-0000000000aa");
    const Guid attack = guid("60000000-0000-4000-8000-0000000000ab");
    const Guid power = guid("60000000-0000-4000-8000-0000000000ac");

    ecs::World worldA;
    gameplay::registerGameplayComponents(worldA);
    ecs::Entity actorA = worldA.create();
    actorA.set<gameplay::AttributeSet>({});
    actorA.set<gameplay::AbilitySystem>({});
    {
        auto& system = actorA.get_mut<gameplay::AbilitySystem>();
        gameplay::grantAbility(system, attack);
        gameplay::grantAbility(system, power);
        gameplay::grantAbility(system, power); // the É6 dedup: no double
        REQUIRE(system.grantedAbilities.size() == 2);
        gameplay::initializeCurrent(system,
                                    actorA.get<gameplay::AttributeSet>());
    }

    // Capture -> resolve (a save is an ordinary plugin, §5) -> apply.
    data::FormTypeRegistry types;
    gameplay::registerSaveFormTypes(types);
    data::Plugin save;
    save.records = gameplay::captureActor(actorA, ref, tags);
    data::FormDatabase db;
    data::resolve({ &save }, types, db);

    const auto saved = gameplay::savedRecordsFor(db, ref);
    REQUIRE(saved.stats != nullptr);
    REQUIRE(saved.abilities.size() == 2);

    ecs::World worldB;
    gameplay::registerGameplayComponents(worldB);
    ecs::Entity actorB = worldB.create();
    actorB.set<gameplay::AttributeSet>({});
    actorB.set<gameplay::AbilitySystem>({});
    gameplay::applySavedState(actorB, saved, tags);
    {
        const auto& granted =
            actorB.get<gameplay::AbilitySystem>().grantedAbilities;
        REQUIRE(granted.size() == 2);
        CHECK(std::find(granted.begin(), granted.end(), attack) !=
              granted.end());
        CHECK(std::find(granted.begin(), granted.end(), power) !=
              granted.end());
    }
    // Re-applying (a spawn-exit perk sync after the restore, a second
    // load) never doubles — grantAbility is idempotent.
    gameplay::applySavedState(actorB, saved, tags);
    CHECK(actorB.get<gameplay::AbilitySystem>().grantedAbilities.size() ==
          2);
}

TEST_CASE("followers É6: the healer's pick — lowest ally below the bar") {
    using gameplay::pickHealTarget;
    const f32 bar = 0.5f;
    // Nobody below the threshold: keep the essence.
    CHECK(pickHealTarget({ { 1, 1.0f }, { 2, 0.5f } }, bar) == 0);
    // The LOWEST fraction wins, not the first found.
    CHECK(pickHealTarget({ { 1, 0.45f }, { 2, 0.2f }, { 3, 0.3f } }, bar) ==
          2);
    // Order-independent (the unordered sweep): same verdict reversed.
    CHECK(pickHealTarget({ { 3, 0.3f }, { 2, 0.2f }, { 1, 0.45f } }, bar) ==
          2);
    // Exact ties break on the smaller id (§8 determinism).
    CHECK(pickHealTarget({ { 7, 0.25f }, { 4, 0.25f } }, bar) == 4);
    // The threshold is strict: exactly-at-the-bar is healthy enough.
    CHECK(pickHealTarget({ { 1, 0.5f } }, bar) == 0);
}

TEST_CASE("followers É6: pickPower skips the shared attack ability") {
    const Guid attack = guid("60000000-0000-4000-8000-0000000000ab");
    const Guid power = guid("60000000-0000-4000-8000-0000000000ac");
    CHECK(gameplay::pickPower({}, attack) == Guid {});
    CHECK(gameplay::pickPower({ attack }, attack) == Guid {});
    CHECK(gameplay::pickPower({ attack, power }, attack) == power);
    CHECK(gameplay::pickPower({ power, attack }, attack) == power);
}
