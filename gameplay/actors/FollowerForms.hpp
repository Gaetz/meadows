#pragma once

#include "data/forms/Form.hpp"
#include "gameplay/stats/CoreAttributes.hpp"

// Follower class & perk data (FOLLOWERS É0 — docs/CHANTIER-FOLLOWERS.md).
// Reuses the existing patterns wholesale: classes are ordinary Forms (§5,
// moddable/patchable), perks are CHILD records keyed by `parent` (the
// LoadoutEntryForm / childrenOf convention), and the curves resolve into
// the existing gameplay::CoreAttributes set — É5 writes them as bases and
// lets the derived-stat pass (efdf8e7 override ?? formula) do the rest.

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

// A follower archetype: what growing as a "war-cry warrior" MEANS. Nine
// linear attribute curves (base + perLevel × (level-1), v1) matching the
// CoreAttributes fields one for one. Defaults mirror CoreAttributes
// defaults (6, insight 0) so an empty class = the standard humanoid.
struct FollowerClassForm : data::Form {
    str displayName;
    str combatStyle; // CombatAi vocabulary (É2/É6); "" = default brain

    f32 strengthBase { 6.0f };
    f32 strengthPerLevel { 0.0f };
    f32 constitutionBase { 6.0f };
    f32 constitutionPerLevel { 0.0f };
    f32 graceBase { 6.0f };
    f32 gracePerLevel { 0.0f };
    f32 dexterityBase { 6.0f };
    f32 dexterityPerLevel { 0.0f };
    f32 alacrityBase { 6.0f };
    f32 alacrityPerLevel { 0.0f };
    f32 perceptionBase { 6.0f };
    f32 perceptionPerLevel { 0.0f };
    f32 charismaBase { 6.0f };
    f32 charismaPerLevel { 0.0f };
    f32 egoBase { 6.0f };
    f32 egoPerLevel { 0.0f };
    f32 insightBase { 0.0f };
    f32 insightPerLevel { 0.0f };

    REFLECT_BEGIN(FollowerClassForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(combatStyle)
        REFLECT_FIELD(strengthBase)
        REFLECT_FIELD(strengthPerLevel)
        REFLECT_FIELD(constitutionBase)
        REFLECT_FIELD(constitutionPerLevel)
        REFLECT_FIELD(graceBase)
        REFLECT_FIELD(gracePerLevel)
        REFLECT_FIELD(dexterityBase)
        REFLECT_FIELD(dexterityPerLevel)
        REFLECT_FIELD(alacrityBase)
        REFLECT_FIELD(alacrityPerLevel)
        REFLECT_FIELD(perceptionBase)
        REFLECT_FIELD(perceptionPerLevel)
        REFLECT_FIELD(charismaBase)
        REFLECT_FIELD(charismaPerLevel)
        REFLECT_FIELD(egoBase)
        REFLECT_FIELD(egoPerLevel)
        REFLECT_FIELD(insightBase)
        REFLECT_FIELD(insightPerLevel)
    REFLECT_END()
};

// A class perk unlocked at a level tier: CHILD record of the class (the
// childrenOf pattern). Grants an ability and/or an infinite effect through
// the existing GAS (§6) — either guid may be null.
struct ClassPerkForm : data::Form {
    core::Guid parent; // FollowerClassForm
    f32 level { 1.0f };
    core::Guid ability; // AbilityForm granted at that level (or null)
    core::Guid effect;  // EffectForm applied at that level (or null)

    REFLECT_BEGIN(ClassPerkForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(level)
        REFLECT_FIELD(ability)
        REFLECT_FIELD(effect)
    REFLECT_END()
};

void registerFollowerFormTypes(data::FormTypeRegistry& registry);

// Resolves the class curves at `level`: base + perLevel × (level - 1),
// linear v1 (levels below 1 clamp to 1). Pure — É5 writes the result into
// the actor's CoreAttributes bases and recomputes.
CoreAttributes classAttributesAt(const FollowerClassForm& cls, f32 level);

} // namespace gameplay
