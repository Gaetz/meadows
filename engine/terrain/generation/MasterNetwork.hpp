#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Stage 0 — the REGIONAL master hydrology: a coarse flow routing over
// the analytic macro (which already carries the B4 trunk valleys, so
// the big drainage concentrates in them by construction). It yields
// the FLEUVE courses with their TRUE drainage areas and monotone
// water surfaces — the per-tile hydrology window (~6 km) truncates
// areas and cannot know a fleuve; this layer can. Consumers: the
// fleuve tier promotion (S4), site scoring (towns at confluences and
// mouths), and later the road network (the same coarse layer is the
// natural cost/routing support).
//
// Purity contract: computeMasterNetwork is a pure function of
// (controls, macro, params, super cell) — no cache, no state, safe on
// bake workers, bit-identical for every caller. Ownership by super
// cell (a river belongs to the cell holding its head) keeps two
// callers' views identical; a course is traced through the apron up
// to `apron` meters beyond its cell, further continuation is the
// known deferred (true cross-super stitching).

namespace render::terraingen {

struct MasterNode {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 surface { 0.0f }; // routed water surface, monotone downstream
    f32 area { 0.0f };    // m² of TRUE drainage at this node
};

struct MasterRiver {
    vector<MasterNode> nodes; // downstream order
    bool reachesSea { false };
};

struct MasterNetworkParams {
    f32 superRegionSize { 24576.0f };
    f32 apron { 8192.0f };
    f32 texel { 128.0f };
    // Drainage area that makes a FLEUVE (the real obstacle tier):
    // ~20 m of half-width through the sqrt(A) law, and low enough
    // that the tier begins well inland (at 1.5e7 the courses were
    // 1-2 km coastal stubs — this world's basins are compact).
    f32 fleuveArea { 6.0e6f };
    f32 seaLevel { kDefaultSeaLevel };
    f32 minSlope { 1.0e-4f };
};

struct MasterNetwork {
    GridSpec grid; // core + apron routing window
    vector<MasterRiver> rivers;
};

MasterNetwork computeMasterNetwork(const ProceduralControls& controls,
                                   const MacroParams& macro,
                                   const MasterNetworkParams& params,
                                   i32 superX, i32 superZ);

// Master rivers of every super cell overlapping the aabb (ownership
// by head cell — deterministic for all callers), pruned to courses
// whose nodes touch the aabb.
vector<MasterRiver> masterRiversNear(const ProceduralControls& controls,
                                     const MacroParams& macro,
                                     const MasterNetworkParams& params,
                                     f32 minX, f32 minZ, f32 maxX,
                                     f32 maxZ);

} // namespace render::terraingen
