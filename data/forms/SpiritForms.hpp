#pragma once

#include "data/forms/Form.hpp"

// Chantier ESPRITS — the DATA side of the spirit framework (§5: every
// definition below is an ordinary Form, layered by load order, patched
// per field by mods). The kernels never see these; world/spirit/
// SpiritRules compiles them into the POD table the fields tick with
// (the Forms -> params mapping TerrainPatches does for HeightPatches).

namespace data {

class FormTypeRegistry;

// One record per spirit — the definition of what the field does. The
// triad/channel pair places it in the shifumi (see
// engine/terrain/SpiritField.hpp); `contactEffect` is the ONLY path from
// a world field to an actor (§2.9: an EffectForm with buildupType
// ignition/glaciation/electrocution/mental/curse/bleed).
struct SpiritForm : Form {
    str name;      // "Water" | "Fire" | ... (matches SpiritKind by name)
    i32 triad { 1 };
    str channel;   // "onyx" | "amber" | "garnet"
    str fieldKind; // "volume" | "coverage" | "vector" | "radiance" |
                   // "conduction" | "activation" | "delta"
    f32 decayPerSecond { 0.0f };     // coverage fade (heat loss, drying)
    f32 spreadRate { 0.0f };         // coverage: damage/s to neighbours
    f32 ignitionPoints { 1.0f };     // hitpoints a cell loses before it turns on
    i32 spreadBudgetPerTick { 64 };  // never "the whole map burns"
    core::Guid contactEffect;        // EffectForm applied to actors in the field
    f32 contactPeriod { 0.5f };      // seconds between applications per actor
    f32 contactDamage { 0.0f };      // typed damage (the spirit's element) dealt
                                     // with each contact through applyDamage
    str damageType;                  // triad 2: "blunt" | "slashing" | "piercing"
    str cueSpawn;                    // "Cue.Spirit.Water.Spawn"
    str cueActive;
    str cueExtinguish;
    core::Guid jetParticles;         // ParticleForm streamed along a jet's
                                     // arc (world/spirit/SpiritJets)
    core::Guid holdParticles;        // ParticleForm of a held volume (the
                                     // control spell's floating blob)
    // Earth: ground rising INTO a character throws it up — launch speed
    // = liftQuadratic x depth², clamped to [liftMin, liftMax] m/s (a brush
    // stroke hops it gently, a mound throws it high, a wall is capped).
    f32 emberSeconds { 45.0f };      // fire: a burnt cell's embers cool over this
    f32 regrowSeconds { 180.0f };    // fire: burnt ground regrows over this (dry;
                                     // wet ground up to 4x faster)
    f32 treeIgnitionSeconds { 6.0f }; // fire: a tree in full surrounding fire catches after
    f32 treeBurnSeconds { 25.0f };    // fire: it burns (canopy going) this long
    f32 treeRegrowSeconds { 900.0f }; // fire: a bare tree's canopy returns over this
    core::Guid emberParticles;       // ParticleForm of the sparks a burning
                                     // cell sheds (beside fieldParticles)
    core::Guid fieldSound;           // SoundForm looped near the field's
                                     // active cells (3D, follows the nearest)
    core::Guid fieldParticles;       // ParticleForm of an ACTIVE field cell
                                     // (flames on a burning cell), placed
                                     // by the scene under an emitter budget
    f32 liftQuadratic { 2.0f };
    f32 liftMin { 1.5f };
    f32 liftMax { 25.0f };

    REFLECT_BEGIN(SpiritForm, Form)
        REFLECT_FIELD(name)
        REFLECT_FIELD(triad)
        REFLECT_FIELD(channel)
        REFLECT_FIELD(fieldKind)
        REFLECT_FIELD(decayPerSecond)
        REFLECT_FIELD(spreadRate)
        REFLECT_FIELD(ignitionPoints)
        REFLECT_FIELD(spreadBudgetPerTick)
        REFLECT_FIELD(contactEffect)
        REFLECT_FIELD(contactPeriod)
        REFLECT_FIELD(contactDamage)
        REFLECT_FIELD(damageType)
        REFLECT_FIELD(cueSpawn)
        REFLECT_FIELD(cueActive)
        REFLECT_FIELD(cueExtinguish)
        REFLECT_FIELD(jetParticles)
        REFLECT_FIELD(holdParticles)
        REFLECT_FIELD(fieldParticles)
        REFLECT_FIELD(emberSeconds)
        REFLECT_FIELD(regrowSeconds)
        REFLECT_FIELD(treeIgnitionSeconds)
        REFLECT_FIELD(treeBurnSeconds)
        REFLECT_FIELD(treeRegrowSeconds)
        REFLECT_FIELD(emberParticles)
        REFLECT_FIELD(fieldSound)
        REFLECT_FIELD(liftQuadratic)
        REFLECT_FIELD(liftMin)
        REFLECT_FIELD(liftMax)
    REFLECT_END()
};

// A SPELL: what an activated ability does to the world, as data —
// docs/SPELLS.md. Child record of the AbilityForm that gates it (cost,
// cooldown, tags, skill stay on the ability, §6): the ability is the
// activation, the spell is the effect. One FORM (verb) applied to one
// ELEMENT (spirit), qualified by the characteristics below. The generic
// caster (world/spirit/Spells + the scene) executes the supported
// (form, element, trajectory) cells; `AbilityForm.script` remains the
// escape hatch for anything the matrix does not cover yet.
struct SpellForm : Form {
    core::Guid parent;        // the AbilityForm this spell belongs to
    str name;                 // LocStringForm key shown by the HUD
    str form { "create" };    // create | destroy | transform | control | understand
    str element { "Water" };  // a spirit name (SpiritKind)
    // Where the effect goes:
    //   self       on the caster
    //   point      at the aimed ground spot (within `range`)
    //   stream     a continuous arc from the hand along the aim (follows it)
    //   projectile one arc from the hand; the effect lands where it falls
    //   line       from the spot aimed at the press to the one at release
    str trajectory { "point" };
    f32 range { 20.0f };      // metres — aim reach (point) or the ballistic
                              // reach at 45° (stream/projectile -> launch speed)
    f32 intensity { 1.0f };   // element units per second (water: m³/s)
    f32 duration { 1.0f };    // seconds the effect persists; -1 = permanent
    str areaShape { "disc" }; // disc | ring (the wide-spring seven taps)
    f32 areaRadius { 2.0f };  // metres
    bool channeled { false }; // hold the key to sustain; released = it ends
    f32 costPeriod { 1.0f };  // channeled: the ability cost is paid again
                              // every costPeriod seconds (unaffordable = ends)
    f32 upkeepScale { 0.25f }; // channeled: the fraction of the activation
                               // cost each upkeep pays

    REFLECT_BEGIN(SpellForm, Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(name)
        REFLECT_FIELD(form)
        REFLECT_FIELD(element)
        REFLECT_FIELD(trajectory)
        REFLECT_FIELD(range)
        REFLECT_FIELD(intensity)
        REFLECT_FIELD(duration)
        REFLECT_FIELD(areaShape)
        REFLECT_FIELD(areaRadius)
        REFLECT_FIELD(channeled)
        REFLECT_FIELD(costPeriod)
        REFLECT_FIELD(upkeepScale)
    REFLECT_END()
};

// The rule table (the BotW chemistry rules): one record per (actor,
// target, verb). `actor` MUST be a spirit — a material can never change
// a material, every interaction routes through a spirit; the compiler
// rejects anything else. `target` is a spirit name or a material class.
// The triad shifumi is the DEFAULT rule (matter > energy > spirit >
// matter, verb "suppress"); records refine or override it, and any pair
// outside a triad interacts only if a record says so.
struct SpiritRuleForm : Form {
    str actor;
    str target;
    str verb; // extinguish|ignite|conduct|block|grow|evaporate|wet|
              // freeze|push|erode|activate|suppress
    f32 rate { 1.0f };
    f32 threshold { 0.0f }; // e.g. water depth above which it applies

    REFLECT_BEGIN(SpiritRuleForm, Form)
        REFLECT_FIELD(actor)
        REFLECT_FIELD(target)
        REFLECT_FIELD(verb)
        REFLECT_FIELD(rate)
        REFLECT_FIELD(threshold)
    REFLECT_END()
};

// The physical properties surfaces never had: one record per material
// class ("grass", "rock", "snow", "sand", "cliff", "wood", "bush",
// "metal"), read by the coverage kernels through the compiled table.
struct SurfaceMaterialForm : Form {
    str materialClass;
    f32 flammability { 0.0f };
    f32 fuel { 0.0f };
    f32 moisture { 0.0f };
    f32 conductivity { 0.0f };
    f32 hardness { 1.0f }; // earth push divides by it

    REFLECT_BEGIN(SurfaceMaterialForm, Form)
        REFLECT_FIELD(materialClass)
        REFLECT_FIELD(flammability)
        REFLECT_FIELD(fuel)
        REFLECT_FIELD(moisture)
        REFLECT_FIELD(conductivity)
        REFLECT_FIELD(hardness)
    REFLECT_END()
};

void registerSpiritFormTypes(FormTypeRegistry& registry);

} // namespace data
