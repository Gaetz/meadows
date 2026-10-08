#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"

// The ZONES (docs/PAYSAGE.md §4 bis, principles 1-3, 6-7): the world is
// a mosaic of ~1 km places, each with an ARCHETYPE (its nature), a
// STOREY (its floor, by steps of stepHeight) and a GRAMMAR (how its
// relief, cliffs, cover and water are drawn). The long carrier decides
// the storey trend, the zone rounds it to its step and holds it; two
// zones of different storeys meet at a WALL (a riser whose width
// follows the step count: a climbable band for one step, an
// escarpment above), the walks' corridors ramp through it. Pure
// functions of (seed, x, z): world-anchored jittered cells, warped
// Voronoi borders, memoized per cell in thread-local storage.

namespace render::terraingen {

// One archetype row (docs/POI-CATALOGUE.md §E): where it may draw, and
// the grammar it draws with. The game ships the table as
// ZoneArchetypeForm records (moddable); defaultZoneArchetypes() is
// the same table in C++ for the tests and the benches.
struct ZoneArchetype {
    str name;
    f32 weight { 1.0f }; // draw weight where the conditions hold
    // Conditions on the world sample at the zone's site.
    f32 minEtage { 0.0f };
    f32 maxEtage { 1.0f };
    f32 minMassif { 0.0f };
    f32 maxMassif { 1.0f };
    f32 minCoast { 0.0f };
    f32 maxCoast { 1.0f };
    f32 minMoisture { 0.0f };
    f32 maxMoisture { 1.0f };
    i32 storeyBias { 0 };     // steps added to the trend (plateau +1, basin -1)
    f32 reliefMul { 1.0f };   // tier relief amplitude
    f32 wavelengthMul { 1.0f };
    f32 terrace { 0.0f };     // cliff-and-bench strength inside the zone
    f32 cliffStep { 30.0f };  // bench height (m)
    f32 hillCrests { 0.0f };  // ridged crest amplitude (m): hill country
    f32 hardBias { 0.0f };
    f32 wetBias { 0.0f };
    f32 coverBias { 0.0f };
    u32 palette { 0 };        // named palette (0 = the climate's own)
    // The zone's own small landform: 0 none, 1 knoll, 2 clearing,
    // 3 pond, 4 rock knob, 5 grove mound.
    u32 piece { 0 };
    f32 pieceHeight { 40.0f }; // meters (depth for a pond)
    f32 pieceRadius { 120.0f };
    // The erosion BUDGET of the zone's interior (meters the fastscape
    // may cut, a floor under the calm/rough rule): 0 = the socle's own
    // (raw); hill country, badlands and ranges dissect.
    f32 erosionCut { 0.0f };
};

struct ZoneParams {
    f32 cellSize { 512.0f }; // dev 2026-10-08: twice the rhythm of 1 km
    f32 jitter { 0.6f };            // site jitter, fraction of the cell
    f32 borderWarp { 150.0f };      // meters of border wander
    f32 borderWarpWavelength { 600.0f };
    f32 stepHeight { 60.0f };       // one storey
    u32 storeys { 4 };              // storeys 0..storeys
    f32 trendWavelength { 4000.0f }; // the long carrier of the storeys
    f32 trendContrast { 1.6f };
    f32 wallWidthOne { 90.0f };     // riser width of a one-step wall
    f32 wallWidthHigh { 60.0f };    // riser width PER STEP of a two-step+ escarpment
    f32 rampWidth { 400.0f };       // the corridors' ramp across a border
    f32 startGap { 0.0f };          // meters past the start ring without storeys (0 = none)
    vector<ZoneArchetype> archetypes; // empty = defaultZoneArchetypes()
};

struct ZoneSample {
    u32 archetype { 0 };        // index into the table
    i32 storey { 0 };
    f32 storeyHeight { 0.0f };       // the stepped floor lift (m)
    f32 storeyHeightSmooth { 0.0f }; // the same, ramped across the border
    f32 wall { 0.0f };               // [0,1] on a border riser
    f32 wallSteps { 0.0f };          // storeys across the nearest border
    f32 borderDist { 0.0f };         // meters to the nearest border
    f32 pieceLift { 0.0f };          // the zone's small landform (m)
    f32 pieceBasin { 0.0f };         // (m)
    f32 pieceClearing { 0.0f };      // [0,1]
    // The grammar (the archetype's, blended over ~150 m at a border).
    f32 reliefMul { 1.0f };
    f32 wavelengthMul { 1.0f };
    f32 terrace { 0.0f };
    f32 cliffStep { 30.0f };
    f32 hillCrests { 0.0f };
    f32 hardBias { 0.0f };
    f32 wetBias { 0.0f };
    f32 coverBias { 0.0f };
    u32 palette { 0 };
    f32 erosionCut { 0.0f };
};

ZoneSample zoneSampleAt(const WorldLayerParams& world, const ZoneParams& zones,
                        f32 x, f32 z);

// The archetype index alone (cheap: no kernels) — the per-texel palette.
u32 zoneArchetypeAt(const WorldLayerParams& world, const ZoneParams& zones,
                    f32 x, f32 z);

const vector<ZoneArchetype>& defaultZoneArchetypes();
const vector<ZoneArchetype>& zoneArchetypesOf(const ZoneParams& zones);

// FNV-1a over every field (archetypes included): memo and cache keys.
u64 hashParams(const ZoneParams& p);

} // namespace render::terraingen
