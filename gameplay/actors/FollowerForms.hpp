#pragma once

#include "data/forms/Form.hpp"
#include "gameplay/stats/CoreAttributes.hpp"

// Follower class & perk data (docs/CHANTIER-FOLLOWERS.md).
// Reuses the existing patterns wholesale: classes are ordinary Forms (§5,
// moddable/patchable), perks are CHILD records keyed by `parent` (the
// LoadoutEntryForm / childrenOf convention), and the curves resolve into
// the existing gameplay::CoreAttributes set — the level-up sync writes
// them as bases and lets the derived-stat pass (override ?? formula) do
// the rest.

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
    str combatStyle; // CombatAi vocabulary; "" = default brain

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
//
// DATA DISCIPLINE — perk `effect`s MUST carry a grantedTag. The sync
// re-runs at spawn, on every level-up and after a load; the EffectForm's
// own grantedTag is the dedup key ("already on the target -> skip") AND
// the thing that makes the dedup survive saves for free (SavedEffectForm
// persists the tag; restore re-adds it). An infinite perk effect WITHOUT
// a grantedTag would stack on every sync — grantPerk skips it and warns.
// The same discipline applies to TaughtPerkForm below.
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

// An affinity reaction: CHILD record of the ActorForm (the
// childrenOf pattern, like ClassPerkForm above). When the named bus event
// fires, an eligible follower whose ActorForm owns matching rules moves
// its FollowerState.followerAffinity by `delta` (clamped ±100 — §2.9:
// affinity is NOT a GAS attribute, it never rides applyEffect). Matching
// mirrors QuestTaskForm: event name equality + optional filterTag the
// event's tag must DESCEND from; the two bools narrow the parties.
struct AffinityRuleForm : data::Form {
    core::Guid parent;   // the ActorForm this rule belongs to
    str event;           // bus event name ("OnParried", "OnHitTaken"…)
    str filterTag;       // optional: event.tag must be a descendant of this
    bool sourcePlayer { false }; // require event.source == the player
    bool targetSelf { false };   // require event.target == this follower
    f32 delta { 0.0f };  // signed affinity change

    REFLECT_BEGIN(AffinityRuleForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(event)
        REFLECT_FIELD(filterTag)
        REFLECT_FIELD(sourcePlayer)
        REFLECT_FIELD(targetSelf)
        REFLECT_FIELD(delta)
    REFLECT_END()
};

// A perk the PLAYER can learn from this follower (the
// reverse direction of docs/FOLLOWERS.md §3): CHILD record of the ActorForm
// (childrenOf, like AffinityRuleForm above). The teaching rides the
// existing dialogue machinery — a dialogue option gated by ConditionForm
// children (affinity, the quiet-place zone tag) fires OnLearnPerk; the
// scene resolves the PARTNER's TaughtPerkForm children and grants the
// first unlearned one to the player (grantPerk — ability and/or effect,
// same grantedTag dedup discipline as ClassPerkForm).
struct TaughtPerkForm : data::Form {
    core::Guid parent;  // the teaching follower's ActorForm
    str displayName;    // the toast line names the perk
    core::Guid ability; // AbilityForm granted to the player (or null)
    core::Guid effect;  // EffectForm applied to the player (or null;
                        //   infinite + grantedTag — the discipline above)

    REFLECT_BEGIN(TaughtPerkForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(ability)
        REFLECT_FIELD(effect)
    REFLECT_END()
};

// The follower-to-follower bond: a data-authored INITIAL
// pair affinity, consumed as a banter gate (BanterForm.minPairAffinity).
// V1 scope (docs/CHANTIER-FOLLOWERS.md): no runtime mutation —
// the doc's evolving matrix comes later; the pure gameplay::pairAffinity
// resolves the pair symmetrically ({a,b} order-free), absent pair = 0.
struct FollowerBondForm : data::Form {
    core::Guid a; // one follower's ActorForm
    core::Guid b; // the other's
    f32 affinity { 0.0f };

    REFLECT_BEGIN(FollowerBondForm, data::Form)
        REFLECT_FIELD(a)
        REFLECT_FIELD(b)
        REFLECT_FIELD(affinity)
    REFLECT_END()
};

// An inter-follower banter (docs/FOLLOWERS.md §6.2): two
// lines, one exchange. TOP-LEVEL record (not a child — the pair spans two
// actors), filtered at runtime by "both of the pair are active and near".
// The anti-repeat clock is the ambient-comment one (default 10 game hours,
// the VendorState hour-stamp idiom); oneShot plays once per session (v1:
// the clock map is runtime-only — stated deviation).
struct BanterForm : data::Form {
    core::Guid a;     // the OPENING follower's ActorForm (says lineA)
    core::Guid b;     // the replying follower's ActorForm (says lineB)
    str lineA;
    str lineB;
    bool oneShot { false };
    f32 minPairAffinity { 0.0f }; // gate on the FollowerBondForm affinity
    f32 cooldownHours { 10.0f };  // the doc's 10-game-hour anti-repeat

    REFLECT_BEGIN(BanterForm, data::Form)
        REFLECT_FIELD(a)
        REFLECT_FIELD(b)
        REFLECT_FIELD(lineA)
        REFLECT_FIELD(lineB)
        REFLECT_FIELD(oneShot)
        REFLECT_FIELD(minPairAffinity)
        REFLECT_FIELD(cooldownHours)
    REFLECT_END()
};

// An ambient one-liner (docs/FOLLOWERS.md §6.1): CHILD
// record of the ActorForm (the AffinityRuleForm idiom). Fires on a
// matching bus event (name + optional filterTag descent + minValue floor
// — OnQuietZone enter is value 1, leave is 0) for an ACTIVE follower,
// never in sneak. Anti-repeat: cooldownHours (the doc's 10 h default) and
// oneShot on a runtime clock map (v1 — resets on load, stated). Ordered
// chaining: with requiresComment set, this comment fires only once its
// prerequisite HAS fired at least minGapHours earlier.
struct CommentForm : data::Form {
    core::Guid parent;  // the commenting follower's ActorForm
    str event;          // bus event name ("OnQuietZone", "OnDeath"…)
    str filterTag;      // optional: event.tag must descend from this
    f32 minValue { 0.0f };       // event.value floor (enter-only = 1)
    // Require event.source == the player (the AffinityRuleForm idiom).
    // Trigger volumes fire per crossing ACTOR: a place comment wants the
    // PLAYER's entry, not the follower's own.
    bool sourcePlayer { false };
    str line;           // the spoken line (authored language — data)
    bool oneShot { false };
    f32 cooldownHours { 10.0f };
    core::Guid requiresComment;  // optional prerequisite CommentForm
    f32 minGapHours { 0.0f };    // minimum hours after the prerequisite

    REFLECT_BEGIN(CommentForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(event)
        REFLECT_FIELD(filterTag)
        REFLECT_FIELD(minValue)
        REFLECT_FIELD(sourcePlayer)
        REFLECT_FIELD(line)
        REFLECT_FIELD(oneShot)
        REFLECT_FIELD(cooldownHours)
        REFLECT_FIELD(requiresComment)
        REFLECT_FIELD(minGapHours)
    REFLECT_END()
};

void registerFollowerFormTypes(data::FormTypeRegistry& registry);

// Resolves the class curves at `level`: base + perLevel × (level - 1),
// linear v1 (levels below 1 clamp to 1). Pure — the level-up sync writes
// the result into the actor's CoreAttributes bases and recomputes.
CoreAttributes classAttributesAt(const FollowerClassForm& cls, f32 level);

} // namespace gameplay
