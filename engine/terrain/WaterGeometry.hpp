#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterBodies.hpp"

// CPU meshers of the BAKED water surfaces (docs/PAYSAGE.md): the local
// lake sheets + river ribbons and the far sheets beyond the streamed
// tiles. Pure functions of the published bodies + terrain params —
// terrain::height is worker-proven (the sim jobs sample it against the
// same immutable snapshots) — so they run on a worker and are pinned by
// a golden hash (tests/WaterGeometryTest.cpp). The renderer only uploads
// the vectors they fill.

namespace render::terrain {

// Vertex layout shared by every mesher here: pos (3) + flow dir*speed
// (2) + {halfWidth, lateral, arcLength, endDist} (4) + material (1).
// The last four are the RIVER UV SPACE the shared shading uses for flow
// mapping and the end-of-course dissolve; on lake quads `lateral` rides
// the shore-foam gate (1 = foam allowed, 0 = calm sheet).
constexpr u32 kWaterVertexFloats = 10;

// Far water: flat lake sheets + river ribbons for the world BEYOND the
// streamed tiles. Collected ON A WORKER by the scene's provider (cached
// .twb reads + the memoized master network), everything in a square.
struct FarWaterSet {
    struct Lake {
        f32 level { 0.0f };
        f32 minX { 0.0f };
        f32 minZ { 0.0f };
        f32 cell { 64.0f }; // coarse mask texel (m)
        u32 w { 0 };
        u32 h { 0 };
        vector<u8> mask;
    };
    struct Ribbon {
        struct Node {
            f32 x { 0.0f };
            f32 z { 0.0f };
            f32 surface { 0.0f };
            f32 halfWidth { 0.0f };
        };
        vector<Node> nodes;
    };
    vector<Lake> lakes;
    vector<Ribbon> ribbons;
};

// One lake: a welded sheet over the basin mask with a Laplacian-rounded
// boundary ring whose corners are probed along the outward normal (bank
// -> grow under it, shelf -> reach it, crest -> flush + pouring skirt,
// sibling piece of the same lake -> grown seam). Maskless lakes
// (hand-authored ponds) are one bbox quad. `lake` MUST be an element of
// `bodies.lakes` (the sibling-seam test compares addresses).
void meshLakeSheet(const LakeSurface& lake, const WaterBodies& bodies,
                   const TerrainParams& params, vector<f32>& verts,
                   vector<u32>& indices);

// One river: a ribbon strip along the curvature-subdivided polyline at
// the baked surface, 1.4x the law half-width so the carved shoulder is
// covered and the depth test cuts the shoreline.
void meshRiverRibbon(const RiverSurface& river, vector<f32>& verts,
                     vector<u32>& indices);

// Every lake then every river of the published set, appended.
void buildLocalWaterGeometry(const WaterBodies& bodies,
                             const TerrainParams& params,
                             vector<f32>& verts, vector<u32>& indices);

// Far sheets in the same vertex format, still-water lanes everywhere
// (flow shimmer is invisible at these distances): one quad per row run
// of covered coarse cells, plain ribbon strips. No shore probes, no
// densify — the coarse cells match the far ground resolution and the
// depth test against it cuts the banks.
void buildFarWaterGeometry(const FarWaterSet& set, vector<f32>& verts,
                           vector<u32>& indices);

} // namespace render::terrain
