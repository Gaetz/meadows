#pragma once

#include "engine/core/Defines.hpp"

// The WORLD LAYER (docs/PAYSAGE.md §7.5): the slow, seed-only fields
// that decide what KIND of country a place is before any relief is
// drawn — continent vs sea, the elevation floor (the étage: plains,
// hills, plateau, high mountain), the massif belts where ranges rise,
// the coast proximity and the climate. Pure functions of (seed, x, z):
// infinite, deterministic, continuous across map lines by construction
// (no cells, no per-map state). ProceduralControls derives every
// control field from one sample of this layer; the local rhythm
// (pieces, crests, beds) is drawn on top of its floor.
//
// Start decree: the map around (startX, startZ) is pulled to low
// temperate land for EVERY seed — the sandbox opens on a meadow, the
// world beyond varies freely.

namespace render::terraingen {

// Carrier units per v unit of the archipelago lattice field: the
// lattice writes `seaThreshold + kContinentGain * v` so the downstream
// consumers (coast belt, inland ramp, sea floor, shore-distance proxy)
// read the same carrier scale as the legacy fbm. 0.5 saturates the
// inland ramp at a continent's heart even for the high provinces
// (coastBand * 5.5 = 0.44 < gain) — lower, the hearts lose their
// mountains. The coast slope: one v unit per this fraction of the
// continent radius (the shore-distance proxy's exact inverse).
inline constexpr f32 kContinentGain = 0.5f;
inline constexpr f32 kContinentCoastSlope = 0.22f;

struct WorldLayerParams {
    u32 seed { 1337 };
    // Continent carrier (sea vs land), warped so coasts wander; the
    // coast detail articulates bays and headlands on the belt only.
    f32 continentWavelength { 45000.0f };
    // Archipelago lattice (docs/PAYSAGE.md §7.8): finite continents on
    // a deterministic grid, ocean between — 0 supers = the legacy
    // infinite fbm carrier above. A continent is hosted by ONE master
    // super-cell (continentSuperSize MUST equal MasterNetworkParams::
    // superRegionSize) every `continentCellSupers` cells, and its LAND
    // is kept inside that cell's half-open CORE by a hard edge guard:
    // every river head then belongs to the host cell and no course
    // needs cross-super stitching (MasterNetwork.hpp ownership).
    u32 continentCellSupers { 1 };
    f32 continentSuperSize { 49152.0f };
    // MUST equal MasterNetworkParams::superOrigin: the lattice is
    // offset so the start sits deep inside core (0, 0), not at its
    // corner (where every start continent clipped on the guard).
    f32 continentLatticeOrigin { -16384.0f };
    f32 continentRadius { 19000.0f };
    f32 continentJitter { 2200.0f };
    f32 continentShapeWavelength { 15000.0f };
    f32 continentShapeAmp { 0.45f };
    // Chance a lattice cell holds a continent at all (E1b): empty
    // cells of open ocean break the grid rhythm. The start cell (0,0)
    // always holds its continent.
    f32 continentChance { 0.8f };
    // Map-border styles read off this layer (MapGrid.hpp, E2): ocean
    // majority = Sea, massif mean above the threshold = Ridges, else
    // Open. false = the legacy hashed 50/50 proposal + Sea veto.
    bool borderFromWorld { true };
    f32 borderMassifMin { 0.45f };
    f32 coastDetailWavelength { 5000.0f };
    f32 coastDetailAmp { 0.12f };
    f32 continentWarpWavelength { 12000.0f };
    f32 continentWarpStrength { 1500.0f };
    f32 seaThreshold { 0.47f };
    f32 coastBand { 0.08f }; // continent units: the shoreline belt
    // The étage (elevation province) and the massif belts.
    f32 etageWavelength { 28000.0f };
    f32 massifWavelength { 26000.0f };
    // The étage table: floor of each province index 0..3 (plains,
    // hills, plateau, high mountain), meters above sea level;
    // ControlSample::tier indexes MacroParams::tiers through it.
    f32 etageAltitude[4] { 0.0f, 150.0f, 450.0f, 1200.0f };
    f32 massifLift { 350.0f }; // extra floor under a massif belt
    // Benches: a mid-scale modulation of the floor (basins, benches,
    // shelves) so the country changes every few kilometers inside one
    // province, not only from province to province.
    f32 benchWavelength { 7000.0f };
    f32 benchAmp { 0.2f }; // +/- fraction of the province altitude
    // Plateaus — the SHORT étage: the floor steps between a few levels
    // every ~3 km (the plain, a plateau a step up, a higher one), an
    // escarpment between two levels; height zones change over a short
    // walk instead of a province. Additive (a low province has them
    // too), off inside the meadow, ramped (not stepped) along a walk's
    // corridor (ControlSample reads baseSmooth there). 0 levels = off.
    f32 plateauWavelength { 3000.0f };
    f32 plateauStep { 120.0f };
    u32 plateauLevels { 0 }; // 0 = off: the zones carry the storeys
    f32 plateauEdge { 0.18f };     // fraction of a level band kept as riser
    f32 plateauStartGap { 1600.0f }; // meters past the meadow before they rise
    // Start decree, three reaches. The ANCHOR: the étage field is
    // re-based so the start reads `startEtage` (a low province), the
    // shift fading out far away — the country around the start keeps
    // its own variation, no basin is dug (a flattened disc in high
    // country was a closed bowl: straight rivers, lakes). The LOW
    // COUNTRY ring: land, no massif, temperate climate. The MEADOW:
    // the first steps are flat and low.
    f32 startX { 4096.0f };
    f32 startZ { 4096.0f };
    f32 startEtage { 0.30f };
    // No start decree: no flattened disc, no étage anchor, no meadow,
    // no low-country ring — the start is the world as drawn. Land at
    // the start is the lattice's job (the start continent's jitter is
    // clamped so the spawn sits inland) plus the spawn probe's; 0 =
    // off for the legacy anchor/meadow knobs kept below.
    f32 anchorRadius { 0.0f };
    f32 anchorFade { 0.0f };
    f32 startRadius { 0.0f };
    f32 startFade { 0.0f };
    // Climate: regional fields plus a continental drift, and the
    // altitude lapse (per km of floor).
    f32 climateWavelength { 9000.0f };
    f32 climateSlowWavelength { 200000.0f };
    f32 lapsePerKm { 0.25f };
    // The COVER: a short selector on the climate so the palette (and
    // with it the ground and the scatter) changes every ~45 s of walk —
    // a place of 700 m, between the 350 m confetti rejected in August
    // and the 3 km regions that never alternate.
    f32 coverWavelength { 600.0f };
    f32 coverAmp { 0.16f };
};

struct WorldSample {
    f32 base { 0.0f };      // the elevation floor (m above sea), < 0 at sea
    f32 baseSmooth { 0.0f }; // the floor with the plateaus ramped, not stepped
    f32 scarp { 0.0f };      // [0,1] on a plateau's escarpment (a wall to hold)
    f32 continent { 0.5f }; // carrier value (sea below seaThreshold)
    f32 etage { 0.0f };     // raw province field [0,1]
    f32 massif { 0.0f };    // [0,1] range belt strength
    f32 coast { 0.0f };     // [0,1] 1 on the shoreline, 0 inland/offshore
    f32 temperature { 0.5f };
    f32 moisture { 0.5f };
    f32 cover { 0.5f }; // [0,1] the short cover selector (700 m)
    bool sea { false };
};

WorldSample worldSampleAt(const WorldLayerParams& p, f32 x, f32 z);

// EVERY field, in one walk, for the consumers that must agree on what
// "the parameters" are: the FNV cache key below and the zone memo's
// fingerprint (ZonePlan.cpp) — a pupitre edit must never serve a stale
// sample. Add a field = add a line HERE, nowhere else. `v.mix(scalar)`.
template <class Visitor>
void visitParams(const WorldLayerParams& p, Visitor& v) {
    v.mix(p.seed);
    v.mix(p.continentWavelength);
    v.mix(p.continentCellSupers);
    v.mix(p.continentSuperSize);
    v.mix(p.continentLatticeOrigin);
    v.mix(p.continentRadius);
    v.mix(p.continentJitter);
    v.mix(p.continentShapeWavelength);
    v.mix(p.continentShapeAmp);
    v.mix(p.continentChance);
    v.mix(p.borderFromWorld);
    v.mix(p.borderMassifMin);
    v.mix(p.coastDetailWavelength);
    v.mix(p.coastDetailAmp);
    v.mix(p.continentWarpWavelength);
    v.mix(p.continentWarpStrength);
    v.mix(p.seaThreshold);
    v.mix(p.coastBand);
    v.mix(p.etageWavelength);
    v.mix(p.massifWavelength);
    for (const f32 a : p.etageAltitude) {
        v.mix(a);
    }
    v.mix(p.massifLift);
    v.mix(p.benchWavelength);
    v.mix(p.benchAmp);
    v.mix(p.plateauWavelength);
    v.mix(p.plateauStep);
    v.mix(p.plateauLevels);
    v.mix(p.plateauEdge);
    v.mix(p.plateauStartGap);
    v.mix(p.startX);
    v.mix(p.startZ);
    v.mix(p.startEtage);
    v.mix(p.anchorRadius);
    v.mix(p.anchorFade);
    v.mix(p.startRadius);
    v.mix(p.startFade);
    v.mix(p.climateWavelength);
    v.mix(p.climateSlowWavelength);
    v.mix(p.lapsePerKm);
    v.mix(p.coverWavelength);
    v.mix(p.coverAmp);
}

// FNV-1a over every field (visitParams): the cache keys.
u64 hashParams(const WorldLayerParams& p);

// Continuous province index 0..3 for a floor altitude (the inverse of
// the étage table), and the table read back at a continuous index.
f32 etageIndexFor(const WorldLayerParams& p, f32 base);
f32 etageAltitudeFor(const WorldLayerParams& p, f32 tier);

// Climate -> biome palette id (the BiomeForm contract: 0 temperate,
// 1 arid, 2 alpine, 3 tundra, 4 subalpine, 5 steppe). Cold beats arid
// beats altitude; temperate is the default.
// `cover` [0,1]: inside the temperate default, the short selector
// picks the heath (4) or the dry meadow (5) variant — the ground and
// the scatter change every ~45 s of walk without leaving the climate.
// `characterPalette` (0 = none): inside the temperate default, the POI
// plan's character region or a clearing names its own palette (dense
// forest 6, bocage 7, clearing 8, marsh 9) before the cover variants.
u8 paletteIdFor(f32 temperature, f32 moisture, f32 base, f32 cover = 0.5f,
                u8 characterPalette = 0);

} // namespace render::terraingen
