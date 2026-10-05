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
    f32 anchorRadius { 12000.0f };
    f32 anchorFade { 24000.0f };
    f32 startLowRadius { 4000.0f };
    f32 startLowFade { 8000.0f };
    f32 startRadius { 1200.0f };
    f32 startFade { 4000.0f };
    // Climate: regional fields plus a continental drift, and the
    // altitude lapse (per km of floor).
    f32 climateWavelength { 9000.0f };
    f32 climateSlowWavelength { 200000.0f };
    f32 lapsePerKm { 0.25f };
};

struct WorldSample {
    f32 base { 0.0f };      // the elevation floor (m above sea), < 0 at sea
    f32 continent { 0.5f }; // carrier value (sea below seaThreshold)
    f32 etage { 0.0f };     // raw province field [0,1]
    f32 massif { 0.0f };    // [0,1] range belt strength
    f32 coast { 0.0f };     // [0,1] 1 on the shoreline, 0 inland/offshore
    f32 temperature { 0.5f };
    f32 moisture { 0.5f };
    bool sea { false };
};

WorldSample worldSampleAt(const WorldLayerParams& p, f32 x, f32 z);

// Continuous province index 0..3 for a floor altitude (the inverse of
// the étage table), and the table read back at a continuous index.
f32 etageIndexFor(const WorldLayerParams& p, f32 base);
f32 etageAltitudeFor(const WorldLayerParams& p, f32 tier);

// Climate -> biome palette id (the BiomeForm contract: 0 temperate,
// 1 arid, 2 alpine, 3 tundra, 4 subalpine, 5 steppe). Cold beats arid
// beats altitude; temperate is the default.
u8 paletteIdFor(f32 temperature, f32 moisture, f32 base);

} // namespace render::terraingen
