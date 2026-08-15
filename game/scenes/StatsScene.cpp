#include "game/scenes/StatsScene.hpp"

#include "game/ui/CharacterStatsPanel.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Injuries.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/ResonanceDecays.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/stats/Survival.hpp"

namespace game {

gameplay::StatModifiers StatsScene::equipmentModifiers() const {
    gameplay::StatModifiers mods;
    if (armorEquipped) {
        gameplay::armorModifiers(sampleArmor, mods);
    }
    return mods;
}

void StatsScene::onEnter() {
    WorldDemoScene::onEnter();
    // The shared character-tick vocabulary (life state, statuses, buildup,
    // stats runtime tags) — one aggregator for every scene.
    gameplay::registerCharacterRuntimeTags(tags);

    // The bench actor, carrying the full character kit.
    player = world.create();
    player.set<gameplay::CoreAttributes>({});
    player.set<gameplay::AttributeSet>({});
    player.set<gameplay::AbilitySystem>({});
    player.set<gameplay::Resonance>({});
    player.set<gameplay::Survival>({});
    player.set<gameplay::StatusBuildup>({});
    player.set<gameplay::CombatState>({});
    player.set<gameplay::Injuries>({});
    player.set<gameplay::ResonanceDecays>({});
    gameplay::initializeCurrent(player.get_mut<gameplay::AbilitySystem>(),
                                player.get<gameplay::AttributeSet>());
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };
    gameplay::initializeActorStats(player, ctx, equipmentModifiers());

    // Sample gear.
    sampleWeapon.slashAttack = 30.0f;
    sampleWeapon.scalingAttribute = "strength";
    sampleWeapon.scalingK = 1.5f;
    sampleWeapon.postureDamage = 15.0f;
    sampleArmor.armorSlash = 20.0f;
    sampleArmor.resistFire = 15.0f;

    // Sample disease (amber/energy) + psychosis (garnet/essence).
    sampleDisease.attribute = "amber";
    sampleDisease.op = "add";
    sampleDisease.magnitude = -15.0f;
    sampleDisease.attribute2 = "constitution";
    sampleDisease.magnitude2 = -2.0f;
    sampleDisease.durationHours = 48.0f;
    sampleDisease.grantedTag = "Status.Diseased.Fever";
    tags.registerTag("Status.Diseased.Fever");

    samplePsychosis.attribute = "garnet";
    samplePsychosis.op = "add";
    samplePsychosis.magnitude = -20.0f;
    samplePsychosis.attribute2 = "ego";
    samplePsychosis.magnitude2 = -2.0f;
    samplePsychosis.durationHours = 72.0f;
    samplePsychosis.grantedTag = "Status.Mental.Phobia";
    tags.registerTag("Status.Mental.Phobia");

    // Sample drug: a stimulant (amber boost, breaks harmony, aftershock).
    sampleDrug.attribute = "amber";
    sampleDrug.op = "add";
    sampleDrug.magnitude = 100.0f;
    sampleDrug.durationHours = 2.0f;
    sampleDrug.grantedTag = "Status.HarmonyBroken";
    sampleDrug.expiryMode = "decay";
    sampleDrug.expiryMagnitude = -30.0f;
    sampleDrug.decayPerHour = 1.0f;
}

void StatsScene::update(f32 dt) {
    WorldDemoScene::update(dt);
    const f64 gameDt = clock.advance(dt);
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };
    gameplay::tickCharacter(player, dt, gameDt, ctx, equipmentModifiers());
}

void StatsScene::drawUi() {
    const gameplay::CharacterTickContext ctx { derived, tags, tuning };
    game::ui::CharacterStatsDemoState demo { sampleWeapon,  sampleArmor,
                                             armorEquipped, sampleDrug,
                                             sampleDisease, samplePsychosis,
                                             rng };
    game::ui::drawCharacterStatsPanel(player, ctx, clock,
                                      equipmentModifiers(), &demo);
}

} // namespace game
