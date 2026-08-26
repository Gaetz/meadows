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
// (controls, macro, params, super cell) — bit-identical for every
// caller. A thread-safe process memo backs it (exact-parameter hits
// only — a memoized pure function stays pure): the fleuve imprint asks
// for the same super cells from every stage-1 window, and recomputing
// them per window multiplied the bake cost. Ownership by super
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

// The network's S1 consumption (the B5 plan's "empreinte le long du
// cours réel", wired at last): the fleuve is CONSTRUCTED into the
// terrain instead of recognized after the fact. Along each master
// course the stamp digs the channel, flattens a bounded alluvial plain
// and imposes a smooth monotone bed gradient — the local hydrology then
// finds this channel by construction (water follows the carving) and
// the tier promotion becomes a confirmation. Lowering-only everywhere:
// the imprint can never dam. The analytic mirror stays UNTOUCHED — the
// network routes on it, so imprinting it would loop.
struct MasterImprintParams {
    f32 minGradient { 0.0015f }; // m/m of reprofiled bed (kills the
                                 // flood-flat "lake" stretches)
    f32 bedDepth { 5.0f };       // m under the reprofiled surface
    f32 plainFactor { 3.0f };    // plain half-width = f * channel
    f32 plainLift { 1.5f };      // plain ceiling above the surface
    f32 maxPlainCut { 14.0f };   // deeper crossings stay gorges
    f32 keepChannel { 0.85f };   // erosion keep inside the channel
    f32 keepPlain { 0.35f };
};
// `extraKeep` (sim.cells(), caller-zeroed) receives the erosion
// protection to max into the bake's keep grid. Width law shared with
// classifyRivers (widthCoef/exponent/fleuveWidthScale) so the imprinted
// channel and the promoted ribbon agree.
void imprintMasterChannels(const GridSpec& sim, MacroResult& macro,
                           vector<f32>& extraKeep,
                           const ProceduralControls& controls,
                           const MacroParams& macroParams,
                           const MasterNetworkParams& network,
                           const MasterImprintParams& imprint,
                           f32 widthCoef, f32 widthExponent,
                           f32 fleuveWidthScale);

} // namespace render::terraingen
