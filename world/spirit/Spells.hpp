#pragma once

#include <optional>

#include "data/forms/SpiritForms.hpp"
#include "engine/core/Defines.hpp"
#include "engine/terrain/SpiritField.hpp"

// Chantier ESPRITS — spells (docs/SPELLS.md): the data SpellForm compiled
// into a POD the caster executes. A spell is ONE form (verb) applied to
// ONE element (spirit) with its characteristics; the (form, element,
// trajectory) matrix says which cells the generic caster implements.
// Headless: the scene supplies aim, hand and world.

namespace world {

enum class SpellVerb : u8 { Create, Destroy, Transform, Control, Understand };
enum class SpellTrajectory : u8 { Self, Point, Stream, Projectile };
enum class SpellArea : u8 { Disc, Ring };

struct SpellSpec {
    SpellVerb verb { SpellVerb::Create };
    render::terrain::SpiritKind element { render::terrain::SpiritKind::Water };
    SpellTrajectory trajectory { SpellTrajectory::Point };
    SpellArea area { SpellArea::Disc };
    f32 range { 20.0f };
    f32 intensity { 1.0f };
    f32 duration { 1.0f };  // -1 = permanent
    f32 areaRadius { 2.0f };
    bool channeled { false };
    f32 costPeriod { 1.0f };
    f32 upkeepScale { 0.25f };
};

std::optional<SpellVerb> parseSpellVerb(std::string_view name);
std::optional<SpellTrajectory> parseSpellTrajectory(std::string_view name);
std::optional<SpellArea> parseSpellArea(std::string_view name);

// Validates and compiles one record; on failure `error` names the field.
std::optional<SpellSpec> compileSpell(const data::SpellForm& form,
                                      str* error = nullptr);

// The launch speed whose 45-degree ballistic reach on level ground is
// `range` metres: v = sqrt(g * range).
f32 launchSpeedForRange(f32 range, f32 gravity = 9.81f);

// Whether the generic caster implements this (verb, element, trajectory)
// cell. False = the ability needs a script (or waits for its brick).
bool spellSupported(const SpellSpec& spec);

// The kernel radius a "disc" area collapses to (the water kernel's own
// disc is ~4 m; wider areas are rings — world/spirit/SpiritSources).
constexpr f32 kSpellDiscMaxRadius = 4.0f;

} // namespace world
