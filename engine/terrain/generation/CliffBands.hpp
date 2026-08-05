#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Cliff bands (docs/CLIFFS.md étage 2). On the finalized fine grid:
// detect the connected steep bands, SHARPEN the wall profile in place
// (mid-face pushed toward vertical, foot and crest eased — the classic
// cliff-over-talus curve a smooth erosion output never produces), and
// extract the FOOT polylines the runtime cliff ribbons extrude from.
// Pure grid math — deterministic per (grid, params), worker-safe.

namespace render::terraingen {

struct CliffBandParams {
    f32 detectStride { 8.0f }; // meters between detection samples
    f32 slopeGrad { 1.12f };   // |grad h| threshold at the stride (~48°)
    u32 minCells { 10 };       // component area gate (stride^2 cells)
    f32 minHeight { 8.0f };    // min crest-foot drop for a KEPT band
    // Profile remap strength (0 = geometry untouched). The remap is
    // t' = t^k / (t^k + (1-t)^k) between the local foot and head — the
    // logistic sharpen: steeper mid-face, flatter approach/crest.
    f32 sharpen { 0.7f };
    f32 sharpenExponent { 1.9f };
    // Skip damp ground (river banks, lake shores): carving a wall
    // through a carved river bed fights the hydrology pass.
    f32 wetnessGate { 0.25f };
};

// `heights` = the fine grid (fineSpec), sharpened IN PLACE. `wetness`
// lives on `coarseSpec` (the finalize mask resolution). Returned
// polylines are clipped to [clipMinX, clipMinX + clipSpan) x
// [clipMinZ, clipMinZ + clipSpan) — the OWNING tile rect, so two
// neighbour bakes never both ship the same wall segment.
vector<render::CliffBand> extractCliffBands(
    const GridSpec& fineSpec, vector<f32>& heights,
    const GridSpec& coarseSpec, const vector<u8>& wetness,
    const CliffBandParams& params, f32 clipMinX, f32 clipMinZ,
    f32 clipSpan);

} // namespace render::terraingen
