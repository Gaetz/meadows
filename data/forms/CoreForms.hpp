#pragma once

#include "data/forms/Form.hpp"

// Early sample form types that exercise the data model. The real gameplay
// roster (containers, doors, effects, abilities...) lands with Phase 3;
// keep these two honest in the meantime.

namespace data {

class FormTypeRegistry;

struct WeaponForm : Form {
    str displayName;
    f32 damage { 10.0f };
    f32 weight { 1.0f };
    i32 goldValue { 0 };
    bool twoHanded { false };
    core::Guid sprite; // asset reference, resolved through the asset DB

    // Typed attack channels (docs/STATS.md §3 Offensive): flat damage per type,
    // scaled at use by (1 + scalingK · <scalingAttribute> %). 0 = the weapon does
    // not deal that type. Per-channel scaling is a later refinement.
    f32 slashAttack { 0.0f };
    f32 pierceAttack { 0.0f };
    f32 bluntAttack { 0.0f };
    f32 fireAttack { 0.0f };
    f32 lightningAttack { 0.0f };
    str scalingAttribute { "strength" };
    f32 scalingK { 1.0f };
    f32 postureDamage { 0.0f };
    // Status buildup applied on hit (docs/STATS.md §3): buildupType routes through
    // parseStatusType ("poison"|"bleed"|"ignition"|…); buildupAmount = points per
    // hit. Empty = no status. Appended last so binary ordinals stay stable.
    str buildupType;
    f32 buildupAmount { 0.0f };
    // 3D world visual (chantier 3, appended — ordinals stable): wired by
    // the universal reflected model/material spawner path. Also the first
    // step of EquipmentVisuals (§C.1): the drawn/sheathed weapon mesh.
    core::Guid model;
    core::Guid material;
    // Chantier P0 A2-A7 (appended — ordinals stable): the blade-touch
    // combat (docs/CHANTIER-P0.md — the VISIBLE blade is what hits).
    f32 bladeLength { 0.9f };     // grip -> tip (m): the hit segment
    f32 hitTolerance { 1.2f };    // blade-length multiplier for the test
    f32 swingWindup { 0.25f };    // seconds: raise
    f32 swingActive { 0.20f };    // seconds: the damaging sweep
    f32 swingRecovery { 0.35f };  // seconds: back to guard
    f32 reach { 2.4f };           // AI engagement distance (A6)
    f32 projectileSpeed { 0.0f }; // > 0 = ranged (A7): launch m/s
    // A7+ (appended — ordinals stable): the ITEM one shot consumes;
    // invalid = no ammo needed. Planted arrows give it back on pickup.
    core::Guid ammo;
    // R7 (appended — ordinals stable): AI pause between attacks, seconds.
    // 0 = the C++ fallback (2.2 ranged / 1.6 melee) so unmodded weapons
    // keep today's behavior.
    f32 attackCooldown { 0.0f };
    // FOLLOWERS É7 (appended — ordinals stable): a follower's BASE-KIT
    // item cannot be transferred out of his inventory (docs/FOLLOWERS.md
    // §5); `upgradesTo` names the next tier the forge dialogue swaps it
    // for (§2.2: Forms never mutate — an upgrade IS a different Form).
    bool unremovable { false };
    core::Guid upgradesTo;

    REFLECT_BEGIN(WeaponForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(damage)
        REFLECT_FIELD(weight)
        REFLECT_FIELD(goldValue)
        REFLECT_FIELD(twoHanded)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(slashAttack)
        REFLECT_FIELD(pierceAttack)
        REFLECT_FIELD(bluntAttack)
        REFLECT_FIELD(fireAttack)
        REFLECT_FIELD(lightningAttack)
        REFLECT_FIELD(scalingAttribute)
        REFLECT_FIELD(scalingK)
        REFLECT_FIELD(postureDamage)
        REFLECT_FIELD(buildupType)
        REFLECT_FIELD(buildupAmount)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(bladeLength)
        REFLECT_FIELD(hitTolerance)
        REFLECT_FIELD(swingWindup)
        REFLECT_FIELD(swingActive)
        REFLECT_FIELD(swingRecovery)
        REFLECT_FIELD(reach)
        REFLECT_FIELD(projectileSpeed)
        REFLECT_FIELD(ammo)
        REFLECT_FIELD(attackCooldown)
        REFLECT_FIELD(unremovable)
        REFLECT_FIELD(upgradesTo)
    REFLECT_END()
};

// A wearable piece (docs/STATS.md §3). `slot` is head/torso/arms/legs. Equipping
// it contributes its mitigation % to the derived stats (via StatModifiers) and
// its weight to encumbrance (later). Exposure fields back the temperature loop
// (later). A new C++ Form type (§2.7), extended by data.
struct ArmorForm : Form {
    str displayName;
    str slot { "torso" };
    f32 armorSlash { 0.0f };
    f32 armorBlunt { 0.0f };
    f32 armorPierce { 0.0f };
    f32 resistFire { 0.0f };
    f32 resistLightning { 0.0f };
    f32 coldExposure { 0.0f }; // exposure reduction — temperature loop (later)
    f32 heatExposure { 0.0f };
    f32 weight { 2.0f };
    core::Guid sprite;
    // The remaining elemental resistances (docs/STATS.md §3). Appended after the
    // original fields so binary ordinals stay stable. resistCold completes the
    // core trio; sonic/chemical/psychic/holy/dark/ether cover the full element set.
    f32 resistCold { 0.0f };
    f32 resistSonic { 0.0f };
    f32 resistChemical { 0.0f };
    f32 resistPsychic { 0.0f };
    f32 resistHoly { 0.0f };
    f32 resistDark { 0.0f };
    f32 resistEther { 0.0f };
    // Status-buildup endurance bonuses (docs/STATS.md §3): raise the matching
    // buildup threshold. The three elemental buildups (ignition/glaciation/
    // electrocution) follow resistFire/Cold/Lightning instead, so no fields here.
    f32 endurancePoison { 0.0f };
    f32 enduranceBleed { 0.0f };
    f32 enduranceMental { 0.0f };
    f32 enduranceDisease { 0.0f };
    f32 enduranceCurse { 0.0f };
    f32 enduranceDeath { 0.0f };
    // Trade value (chantier 4 barter, appended — ordinals stable).
    i32 goldValue { 0 };
    // FOLLOWERS É7 (appended): follower base-kit lock — see WeaponForm.
    bool unremovable { false };

    REFLECT_BEGIN(ArmorForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(slot)
        REFLECT_FIELD(armorSlash)
        REFLECT_FIELD(armorBlunt)
        REFLECT_FIELD(armorPierce)
        REFLECT_FIELD(resistFire)
        REFLECT_FIELD(resistLightning)
        REFLECT_FIELD(coldExposure)
        REFLECT_FIELD(heatExposure)
        REFLECT_FIELD(weight)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(resistCold)
        REFLECT_FIELD(resistSonic)
        REFLECT_FIELD(resistChemical)
        REFLECT_FIELD(resistPsychic)
        REFLECT_FIELD(resistHoly)
        REFLECT_FIELD(resistDark)
        REFLECT_FIELD(resistEther)
        REFLECT_FIELD(endurancePoison)
        REFLECT_FIELD(enduranceBleed)
        REFLECT_FIELD(enduranceMental)
        REFLECT_FIELD(enduranceDisease)
        REFLECT_FIELD(enduranceCurse)
        REFLECT_FIELD(enduranceDeath)
        REFLECT_FIELD(goldValue)
        REFLECT_FIELD(unremovable)
    REFLECT_END()
};

// A consumable (food / drug / treatment). It applies `effect` (a GameplayEffect
// guid) when used; the drug/treatment semantics (harmony break, aftershock,
// injury cure) are wired by the Phase-7 mechanics that consume it.
struct ConsumableForm : Form {
    str displayName;
    str category { "food" }; // food | drug | treatment
    core::Guid effect;       // the GameplayEffect applied on use
    f32 weight { 0.1f };
    // 3D world visual (chantier 3, appended — ordinals stable).
    core::Guid model;
    core::Guid material;
    // Chantier 4 (appended): survival restoration on use (needs are
    // component fields, not attributes — the sleep()/rest precedent) and
    // trade value for the barter screen.
    f32 restoreHunger { 0.0f };
    f32 restoreThirst { 0.0f };
    i32 goldValue { 0 };
    // FOLLOWERS É7 (appended): follower base-kit lock — see WeaponForm.
    bool unremovable { false };

    REFLECT_BEGIN(ConsumableForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(category)
        REFLECT_FIELD(effect)
        REFLECT_FIELD(weight)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(restoreHunger)
        REFLECT_FIELD(restoreThirst)
        REFLECT_FIELD(goldValue)
        REFLECT_FIELD(unremovable)
    REFLECT_END()
};

// A plain tradable/carryable item with no behavior — Skyrim's MISC record
// (chantier 4): gold coins, trinkets, crafting junk. New kinds of behavior
// come from effects/scripts, not new component types (§2.7).
struct MiscItemForm : Form {
    str displayName;
    f32 weight { 0.1f };
    i32 goldValue { 0 };
    core::Guid model;
    core::Guid material;
    // FOLLOWERS É7 (appended): follower base-kit lock — see WeaponForm.
    bool unremovable { false };

    REFLECT_BEGIN(MiscItemForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(weight)
        REFLECT_FIELD(goldValue)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(unremovable)
    REFLECT_END()
};

struct ActorForm : Form {
    str displayName;
    f32 maxHealth { 100.0f };
    f32 walkSpeed { 3.0f };
    core::Guid sprite;
    // 3D-demo hooks (horizontal pass H1), appended so binary ordinals stay
    // stable. All optional (0 = the 2D/legacy path).
    core::Guid appearance; // gameplay::AppearanceForm (modular visual)
    core::Guid animGraph;  // AnimGraphForm (animation controller)
    core::Guid schedule;   // gameplay::ScheduleForm (daily routine)
    // Chantier 4 B4 (appended — §C.1 mapping): the conversation opened by
    // [E] Talk (a quest::DialogueForm guid; invalid = a placeholder line).
    core::Guid dialogue;
    // Chantier 6 D1 (appended): per-vendor barter multipliers.
    // 0 = use the global StatsTuningForm barterBuyMult/barterSellMult.
    f32 buyMult { 0.0f };
    f32 sellMult { 0.0f };
    // Chantier P0 B3 (appended): grit in combat — the actor flees below
    // (1 - courage) of its max health (0.75 = runs under 25%).
    f32 courage { 0.75f };
    // Brain script (appended — docs/BOSS-SCRIPTING.md, dev 2026-07-11):
    // Lua source RETURNING a decide(situation) -> move-name function,
    // called on low-frequency decision ticks. Empty = the C++ brain
    // (chooseCombatMove). Works for ANY hostile actor, boss or mugger.
    str brainScript;
    // FOLLOWERS É0 (appended — docs/CHANTIER-FOLLOWERS.md): the authored
    // follower identity. All optional (empty/0 = not a follower); the
    // runtime state lives in gameplay::FollowerState, never here (§2.2).
    str followerCategory;      // "" | "major" | "minor" | "mount"
    core::Guid followerClass;  // gameplay::FollowerClassForm (level curves)
    f32 age { 0.0f };          // years; 0 = ageless (no age effects, É5)
    f32 minLevel { 1.0f };     // recruit gate (condition evaluator, É4)
    bool mainCharacter { false }; // full level catch-up on re-recruit (É5)
    core::Guid homeMarker;     // where a dismissed follower returns (É1)
    core::Guid recruitDialogue; // if distinct from `dialogue` (É1)
    core::Guid buryMarker;     // authored grave spot (É8)
    core::Guid buryContact;    // NPC who buries a nearby dead follower (É8)

    REFLECT_BEGIN(ActorForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(maxHealth)
        REFLECT_FIELD(walkSpeed)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(appearance)
        REFLECT_FIELD(animGraph)
        REFLECT_FIELD(schedule)
        REFLECT_FIELD(dialogue)
        REFLECT_FIELD(buyMult)
        REFLECT_FIELD(sellMult)
        REFLECT_FIELD(courage)
        REFLECT_FIELD(brainScript)
        REFLECT_FIELD(followerCategory)
        REFLECT_FIELD(followerClass)
        REFLECT_FIELD(age)
        REFLECT_FIELD(minLevel)
        REFLECT_FIELD(mainCharacter)
        REFLECT_FIELD(homeMarker)
        REFLECT_FIELD(recruitDialogue)
        REFLECT_FIELD(buryMarker)
        REFLECT_FIELD(buryContact)
    REFLECT_END()
};

// Registers every core form type; call once at startup before loading
// plugins. Mods cannot add form *types* (§2.7) — richness comes from data
// on existing types, effects, and scripts.
void registerCoreFormTypes(FormTypeRegistry& registry);

} // namespace data
