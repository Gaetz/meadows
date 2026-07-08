#include <doctest/doctest.h>

#include "engine/ecs/World.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"    // attr, currentValueOf
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/combat/Combat.hpp"          // updateLifeState
#include "gameplay/save/SaveState.hpp"
#include "gameplay/stats/CharacterStats.hpp"   // registerStatsComponents, recomputeStats
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"           // CombatState
#include "gameplay/stats/GameTime.hpp"         // tickGameTime, GameTimeTickArgs
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatsTuning.hpp"      // StatsTuningForm
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

// Chantier audit U4-10 follow-up: a killed NPC must stay dead across a cell
// unload/reload (the open-world state is a §5 patch layer). The scene path is
// capture-on-unload (captureActor) -> respawn -> finalizeActorSpawn
// (initializeActorStats resets vitals to full, THEN applySavedState restores
// the captured base health 0 and re-derives State.Dead). This exercises that
// data contract directly — the piece CellDeltaTest never covered (it only
// checked a wounded, still-alive actor).

using namespace gameplay;
using core::Guid;

namespace {

const Guid kBanditRef =
    *Guid::fromString("4d7a9b30-0000-4000-8000-000000000094");

// A humanoid stat sheet: str+con+grace = 60 -> maxHealth 300 (attributeToMax 5).
void seedHumanoid(ecs::Entity e) {
    CoreAttributes core;
    core.strength = core.constitution = core.grace = 20.0f;
    core.dexterity = core.alacrity = core.perception = 20.0f;
    core.charisma = core.ego = core.insight = 20.0f;
    e.set<CoreAttributes>(core);
    e.set<AttributeSet>({});
    e.set<AbilitySystem>({});
    e.set<CombatState>({});
    // The rest of the character-state components a full tickCharacter reads.
    e.set<StatusBuildup>({});
    e.set<Resonance>({});
    e.set<Survival>({});
    e.set<Injuries>({});
    e.set<ResonanceDecays>({});
}

// Seed currents from bases, then fill health to full (the "alive, at max" state
// that initializeActorStats produces on spawn).
void reviveToFull(ecs::Entity e, const DerivedStatRegistry& derived,
                  const GameplayTagRegistry& tags) {
    auto& sys = e.get_mut<AbilitySystem>();
    recomputeStats(e.get<CoreAttributes>(), e.get_mut<AttributeSet>(), sys,
                   derived, nullptr);
    e.get_mut<AttributeSet>().health =
        currentValueOf(sys, attr("maxHealth"));
    recomputeStats(e.get<CoreAttributes>(), e.get_mut<AttributeSet>(), sys,
                   derived, nullptr);
    updateLifeState(sys, tags);
}

} // namespace

TEST_CASE("death persists across an unload/reload (capture -> respawn -> apply)") {
    ecs::World world;
    registerGameplayComponents(world); // AttributeSet, AbilitySystem
    registerStatsComponents(world);    // CoreAttributes, CombatState, ...

    DerivedStatRegistry derived;
    registerCoreDerivedStats(derived);
    GameplayTagRegistry tags;
    const GameplayTag deadTag = tags.registerTag("State.Dead");

    // --- The bandit lives, then the player kills it -------------------------
    ecs::Entity bandit = world.handle().entity();
    seedHumanoid(bandit);
    reviveToFull(bandit, derived, tags);
    REQUIRE_FALSE(bandit.get<AbilitySystem>().tags.has(deadTag));

    // Lethal blow: base health to zero, re-derive the life state (the §2.9
    // execution-calc terminal write the damage path performs).
    bandit.get_mut<AttributeSet>().health = 0.0f;
    {
        auto& sys = bandit.get_mut<AbilitySystem>();
        recomputeStats(bandit.get<CoreAttributes>(),
                       bandit.get_mut<AttributeSet>(), sys, derived, nullptr);
        updateLifeState(sys, tags);
    }
    REQUIRE(bandit.get<AbilitySystem>().tags.has(deadTag));

    // --- Cell unloads: capture the corpse's state (beforeUnload path) -------
    const std::vector<data::Record> records =
        captureActor(bandit, kBanditRef, tags);

    // The captured stat sheet must carry the zero base health (else nothing
    // makes the reload dead). Reconstruct it as the pending layer does.
    SavedStatsForm statsForm;
    bool sawStats = false;
    for (const data::Record& record : records) {
        if (record.typeId == SavedStatsForm::staticTypeInfo().id) {
            statsForm = formFromRecord<SavedStatsForm>(record);
            sawStats = true;
        }
    }
    REQUIRE(sawStats);
    CHECK(statsForm.health == doctest::Approx(0.0f));

    SavedActorRecords saved;
    saved.stats = &statsForm;

    // --- Cell reloads: a FRESH entity spawns at full health, then the scene
    //     applies the captured state (finalizeActorSpawn) --------------------
    ecs::Entity reloaded = world.handle().entity();
    seedHumanoid(reloaded);
    reviveToFull(reloaded, derived, tags); // spawnActor + initializeActorStats
    REQUIRE_FALSE(reloaded.get<AbilitySystem>().tags.has(deadTag));

    applySavedState(reloaded, saved, tags);

    // The bandit must load dead: base health 0, current 0, State.Dead set.
    CHECK(reloaded.get<AttributeSet>().health == doctest::Approx(0.0f));
    CHECK(currentValueOf(reloaded.get<AbilitySystem>(), attr("health")) <=
          0.0f);
    CHECK(reloaded.get<AbilitySystem>().tags.has(deadTag));
}

// The real-world root cause: the scene ticks a corpse every frame (tickCharacter
// runs before the NpcDirector dead-check), and game-time health regen used to be
// ungated — the dead actor's BASE health slowly climbed back over 0. In place it
// stayed down (nothing re-derived life state), but the capture then stored a
// POSITIVE health, so the reload came back ALIVE. Regen must be gated by death.
// Tested at the game-time tick (where the regen lives) with plain stat structs.
TEST_CASE("a slain actor does not regenerate back to life") {
    DerivedStatRegistry derived;
    registerCoreDerivedStats(derived);
    GameplayTagRegistry tags;
    const GameplayTag deadTag = tags.registerTag("State.Dead");
    const StatsTuningForm tuning;

    CoreAttributes core;
    core.strength = core.constitution = core.grace = 20.0f; // grace 20 -> regen
    core.dexterity = core.alacrity = core.perception = 20.0f;
    core.charisma = core.ego = core.insight = 20.0f;
    AttributeSet vitals;
    AbilitySystem system;
    CombatState combat;
    StatusBuildup buildup;
    Survival survival;
    Injuries injuries;
    Resonance resonance;
    ResonanceDecays resoDecays;

    // Kill: base health to 0, State.Dead derived.
    recomputeStats(core, vitals, system, derived, nullptr);
    vitals.health = 0.0f;
    recomputeStats(core, vitals, system, derived, nullptr);
    updateLifeState(system, tags);
    REQUIRE(system.tags.has(deadTag));

    // Many game-hours of ticking — ungated healthRegen would heal it to full.
    GameTimeTickArgs args { core,     vitals,   system,    combat,
                            buildup,  survival, injuries,  resonance,
                            resoDecays, derived, tags,     tuning };
    const StatModifiers mods;
    for (int i = 0; i < 240; ++i) {
        tickGameTime(args, /*gameDt=*/3600.0, mods);
    }

    CHECK(vitals.health == doctest::Approx(0.0f)); // regen gated by death
    CHECK(system.tags.has(deadTag));               // nothing revived it in place
}
