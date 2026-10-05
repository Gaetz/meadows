#pragma once

#include <functional>
#include <optional>

#include "engine/terrain/generation/TerrainGen.hpp"

// Generated-world identity ("sandbox" = generated, as opposed to the
// story demo noise): the seed-derived control/macro parameters, the map
// lattice and the active map's overview. When TerrainParams.sandbox is
// set, the procedural fallback OUTSIDE published slices becomes the
// map overview where it exists, else the analytic S1 macro
// (macroHeightAnalytic) — so FarTerrain silhouettes, unbaked maps and
// not-yet-streamed ground agree with what the bake produces. Null = the
// legacy demo noise (the story world stays bit-identical).

namespace render {

struct SandboxTerrain {
    terraingen::ProceduralControlParams controls;
    terraingen::MacroParams macro;
    // Bounded-map border transitions (chantier CARTES v2): the
    // analytic fallback applies the same border-line shaping the bakes
    // use — one pure function of (seed, lattice), so the horizon,
    // every unbaked map and the baked rims agree by construction.
    terraingen::MapGridSpec grid;
    // The baked map's 64 m OVERVIEW (decimated global stage-1, rim
    // included): the fallback INSIDE its coverage — a pointwise
    // analytic mirror cannot follow a globally carved valley network
    // (measured hundreds of meters of drift); the map's own coarse
    // truth can. Empty = analytic fallback (legacy / unbaked map).
    terraingen::GridSpec overviewGrid;
    vector<f32> overview;
    // The cached NEIGHBOUR maps' overviews: at 8 km maps the horizon is
    // mostly neighbour ground, so the fallback reads them after the
    // active one (loaded by the scene as neighbours bake; a map's own
    // overview covers its rect + apron, the active one wins where they
    // overlap).
    struct Overview {
        i32 mapX { 0 };
        i32 mapZ { 0 };
        terraingen::GridSpec grid;
        vector<f32> heights;
    };
    vector<Overview> neighbourOverviews;
};

// The fallback ground of the generated world at (x, z): the map's 64 m
// overview (bilinear) inside its coverage, else the analytic S1 macro
// shaped by the border lattice. ONE implementation shared by the
// runtime height seam (TerrainNoise::proceduralBase), the spawn probe
// below and the headless diagnostics — they must never disagree.
f32 sandboxFallbackHeight(const SandboxTerrain& sb, f32 x, f32 z);

// The start criterion of a generated map: low, gentle, TEMPERATE land
// (the start is a green meadow by decree — temperate is the default
// biome, always findable). Shared by the game's probe and the hidden
// `biome locator` diagnostic so the two never drift apart.
bool spawnCandidateOk(f32 h, f32 seaLevel, u8 biome);

// Probes the start of map (mapX, mapZ): a spiral anchored on the map
// centre, staying inside the rim band, returning the first candidate
// that passes spawnCandidateOk on the fallback ground (the overview
// when the map is baked — the analytic drifts by hundreds of meters
// against a global erosion, a spot picked on it can sit in a real
// lake). With no such candidate (a plateau or alpine map), the
// gentlest dry spot over the same rings -- never a slope. `wet`: the
// caller's water oracle (the cached lakes/ribbons of the map; a lake
// bed is the flattest ground of all and must never win); empty = sea
// level only. nullopt = nothing dry at all; the caller starts at the
// centre.
std::optional<Vec3> probeMapSpawn(
    const SandboxTerrain& sb, i32 mapX, i32 mapZ, f32 seaLevel,
    const std::function<bool(f32 x, f32 z)>& wet = {});

} // namespace render
