#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TileBake.hpp"

// The fine re-leveling of a map's hydrology (phase B of the two-phase
// map bake, TileBake.hpp): extractMapHydrology routes lakes and
// courses on the window ground, then hands the result to
// relevelMapHydrology, which REBUILDS every course profile from the
// fine pre-carve ground — the routing surface is a chain of flat
// pools at spill levels and the window overstates incised floors, so
// a clamp-only seat turned every descent into stairs with lips whose
// side-water drains away. The passes run in a fixed order (see
// MapHydrology.cpp, kRelevelPasses) and never move a point's position
// after the retrace: any post-smoothing displacement re-creates
// crenellation (measured, docs/PAYSAGE.md).

namespace render::terraingen {

// The hydrology window's ground sampler: the FINAL (phase-A) slice
// ground where a slice covers the point (the slice holding it DEEPEST
// inside wins — the outer kFineErosionHalo ring is edge-effect
// territory), a cached neighbour map's published ground past the map
// rect when one is given, else the stage-1, blended over ~192 m so the
// flood sees no step along the coverage edge. Pure: reads only the
// referenced inputs, which must outlive the sampler.
struct MapGroundSampler {
    const TileStage1& mapS1;
    const vector<const FinalizeResult*>& groundSlices;
    const vector<render::TerrainRegion>& neighbourRegions;
    f32 mapMinX; // map rect origin (world)
    f32 mapMinZ;
    f32 mapSize; // map rect side (world)

    f32 at(f32 x, f32 z) const;
};

// Activity counters of the corrective passes, logged by
// extractMapHydrology: the measure that decides which correction
// survives a routing change.
struct HydroRelevelStats {
    size_t recenterMoved { 0 }; // points shifted by the re-center
    size_t retracedRuns { 0 };  // buried runs the corridor replaced
    f64 fillShift { 0.0 };      // |profile fill - routed surface|, m
    size_t poolLakes { 0 };     // discovered pools emitted as lakes
    size_t saddleRuns { 0 };    // short buried runs the cut grounded
};

// Runs the re-leveling passes on `out` (courses, lakes, pierceDelta
// and ground are edited in place). `hydrology` is params.hydrology
// with seaLevel resolved; `preGround` is the window ground BEFORE the
// outlet piercing (the saddle cut writes its notches relative to it);
// `lap(name)` is called once after each pass group, in order, with
// the phase names the hydrology log line carries.
HydroRelevelStats relevelMapHydrology(
    const TileBakeParams& params, const HydrologyParams& hydrology,
    const MapGroundSampler& ground, const vector<f32>& preGround,
    MapHydrology& out, const std::function<void(const char*)>& lap);

} // namespace render::terraingen
