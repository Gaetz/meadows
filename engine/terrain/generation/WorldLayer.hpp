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

struct WorldLayerParams {
    u32 seed { 1337 };
    // Continent carrier (sea vs land), warped so coasts wander; the
    // coast detail articulates bays and headlands on the belt only.
    f32 continentWavelength { 45000.0f };
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
    // Dev decision 2026-10-08: no flattened start disc, no étage
    // anchor, no meadow — the start is the world as drawn. Only a
    // short LOW-COUNTRY ring remains (land, no massif, temperate) so
    // the spawn probe finds dry ground; 0 = off.
    f32 anchorRadius { 0.0f };
    f32 anchorFade { 0.0f };
    f32 startLowRadius { 4000.0f };
    f32 startLowFade { 4000.0f };
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

// FNV-1a over EVERY field (the memo keys: a pupitre edit must never
// serve a stale sample). Add a field = add a line here.
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
