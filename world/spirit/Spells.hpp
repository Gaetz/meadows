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
// Line: from the ground spot aimed when the key is pressed to the one
// aimed when it is released (a wall drawn across the ground).
enum class SpellTrajectory : u8 { Self, Point, Stream, Projectile, Line };
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

// What a supported cell does — the handler id the scene dispatches on.
// The Readings produce no world action; EarthWall is drawn press to
// release; a Water Destroy is the Source with its rate negated (the
// scene keys that on the verb).
enum class SpellEffect : u8 {
    WaterSource,
    WaterJet,
    WaterHold,
    WaterReading,
    EarthBump,
    EarthBrush,
    EarthDig,
    EarthWall,
    EarthSeize,
    EarthReading,
    FireIgnite,
    FireStream,
    FireDouse,
    FireGlobe,
    FireBrand,
    FireReading,
    WindGust,
    WindBlow,
    WindCalm,
    WindDirect,
};

// Whether a cell takes the spell held (channeled), as a one-shot, or both.
enum class Channel : u8 { Any, Held, Instant };

struct SpellCell {
    render::terrain::SpiritKind element;
    SpellVerb verb;
    SpellTrajectory trajectory;
    Channel channel;
    SpellEffect effect;
};

// The ONE (element, verb, trajectory, channel) matrix the generic caster
// implements (docs/SPELLS.md §4): spellSupported and the scene's
// dispatch both read it. A combination absent here needs a script (or
// waits for its brick). Projectile has no cell yet.
inline constexpr SpellCell kSpellMatrix[] = {
    // Water
    { render::terrain::SpiritKind::Water, SpellVerb::Create, SpellTrajectory::Point,
      Channel::Any, SpellEffect::WaterSource },
    { render::terrain::SpiritKind::Water, SpellVerb::Destroy, SpellTrajectory::Point,
      Channel::Any, SpellEffect::WaterSource }, // draining: the rate negated
    { render::terrain::SpiritKind::Water, SpellVerb::Create, SpellTrajectory::Stream,
      Channel::Any, SpellEffect::WaterJet },
    { render::terrain::SpiritKind::Water, SpellVerb::Control, SpellTrajectory::Point,
      Channel::Held, SpellEffect::WaterHold },
    { render::terrain::SpiritKind::Water, SpellVerb::Understand, SpellTrajectory::Point,
      Channel::Any, SpellEffect::WaterReading },
    // Earth
    { render::terrain::SpiritKind::Earth, SpellVerb::Create, SpellTrajectory::Point,
      Channel::Instant, SpellEffect::EarthBump },
    { render::terrain::SpiritKind::Earth, SpellVerb::Create, SpellTrajectory::Point,
      Channel::Held, SpellEffect::EarthBrush },
    { render::terrain::SpiritKind::Earth, SpellVerb::Create, SpellTrajectory::Line,
      Channel::Any, SpellEffect::EarthWall },
    { render::terrain::SpiritKind::Earth, SpellVerb::Destroy, SpellTrajectory::Point,
      Channel::Held, SpellEffect::EarthDig },
    { render::terrain::SpiritKind::Earth, SpellVerb::Control, SpellTrajectory::Point,
      Channel::Held, SpellEffect::EarthSeize },
    { render::terrain::SpiritKind::Earth, SpellVerb::Understand, SpellTrajectory::Point,
      Channel::Any, SpellEffect::EarthReading },
    // Fire
    { render::terrain::SpiritKind::Fire, SpellVerb::Create, SpellTrajectory::Point,
      Channel::Any, SpellEffect::FireIgnite },
    { render::terrain::SpiritKind::Fire, SpellVerb::Create, SpellTrajectory::Stream,
      Channel::Held, SpellEffect::FireStream },
    { render::terrain::SpiritKind::Fire, SpellVerb::Destroy, SpellTrajectory::Point,
      Channel::Any, SpellEffect::FireDouse },
    { render::terrain::SpiritKind::Fire, SpellVerb::Destroy, SpellTrajectory::Self,
      Channel::Held, SpellEffect::FireGlobe },
    { render::terrain::SpiritKind::Fire, SpellVerb::Control, SpellTrajectory::Point,
      Channel::Held, SpellEffect::FireBrand },
    { render::terrain::SpiritKind::Fire, SpellVerb::Understand, SpellTrajectory::Point,
      Channel::Any, SpellEffect::FireReading },
    // Wind
    { render::terrain::SpiritKind::Wind, SpellVerb::Create, SpellTrajectory::Point,
      Channel::Any, SpellEffect::WindGust },
    { render::terrain::SpiritKind::Wind, SpellVerb::Create, SpellTrajectory::Stream,
      Channel::Held, SpellEffect::WindBlow },
    { render::terrain::SpiritKind::Wind, SpellVerb::Destroy, SpellTrajectory::Self,
      Channel::Held, SpellEffect::WindCalm },
    { render::terrain::SpiritKind::Wind, SpellVerb::Control, SpellTrajectory::Self,
      Channel::Held, SpellEffect::WindDirect },
};

// The cell a spec falls in, nullptr when the matrix has none (= not
// supported).
const SpellCell* spellCell(const SpellSpec& spec);

// Whether the generic caster implements this (verb, element, trajectory)
// cell: spellCell(spec) != nullptr.
bool spellSupported(const SpellSpec& spec);

// The kernel radius a "disc" area collapses to (the water kernel's own
// disc is ~4 m; wider areas are rings — world/spirit/SpiritSources).
constexpr f32 kSpellDiscMaxRadius = 4.0f;

} // namespace world
