#pragma once

#include <filesystem>

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

// Node flags (the lake bit lands with E5).
constexpr u32 kMasterNodeLake = 1u << 0;

struct MasterNode {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 surface { 0.0f }; // routed water surface, monotone downstream
    f32 area { 0.0f };    // m² of TRUE drainage at this node
    // m² of EFFECTIVE drainage: cell areas weighted by the climate
    // runoff (passe EAU E4a) — what the width/tier laws read. Equals
    // `area` when the weights are off or neutral.
    f32 qArea { 0.0f };
    u32 flags { 0 };    // kMasterNode* (E5: lake-crossing nodes)
    u32 headType { 0 }; // E6: source typology of the course's head
};

struct MasterRiver {
    vector<MasterNode> nodes; // downstream order
    bool reachesSea { false };
};

struct MasterNetworkParams {
    f32 superRegionSize { 24576.0f };
    f32 apron { 8192.0f };
    f32 texel { 128.0f };
    // Routing ground (passe EAU E3): with a valid spec the network
    // routes on the border-SHAPED surface (applyMapGridShape) — the
    // ridge lines are real divides and courses cross them at their
    // cols instead of ignoring walls the bake then raises around them.
    // Invalid spec or flag off = the bare analytic (tests, benches,
    // the POI plan-free network, the map-export image).
    MapGridSpec mapGrid {};
    bool routeOnShaped { true };
    // Climate runoff weights (E4a): per-cell multiplier
    // mix(runoffDryMin, runoffWetMax, moisture) * (1 + snowBonus *
    // coldness) accumulated into qArea — wet or snow-fed mountains
    // carry more water than their bare area, dry plains less. Neutral
    // around 1 in temperate country so the thresholds keep their
    // meaning. Off = qArea == area, bit-exact.
    bool runoffWeights { true };
    f32 runoffDryMin { 0.4f };
    f32 runoffWetMax { 1.6f };
    f32 snowBonus { 0.8f };
    // Temperature below which the head country reads snow-fed (the
    // coldness smoothstep spans +0.15 above it).
    f32 snowlineTemp { 0.25f };
    // qArea that EXTRACTS a master course (E4b): low enough that the
    // courses are traced from the mountains (a ~5 m half-width head
    // through the width law — the readable minimum at the 128 m
    // texel); the continuum ruisseau → rivière → fleuve then lives
    // along ONE course instead of beginning at the fleuve threshold.
    f32 riverExtractArea { 1.5e6f };
    // qArea of the FLEUVE tier (the real obstacle): no longer the
    // extraction threshold, only where tier 2 begins along a course.
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

// The DISK cache of the super-cell networks (docs/CPU-PERF.md, Z1): a
// network is a pure function of (params, super cell) and costs seconds
// of analytic sampling (6-7 s per map at 6 us a sample) — paid by
// every bake (the imprint) and every load (the far water, the spawn
// probe) until written here once, keyed on every parameter. Set by
// the game and the cooker to terrain-cache/<seed>; empty = the
// in-memory memo only (tests, benches).
void setMasterNetworkCacheDir(const std::filesystem::path& dir);

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
// classifyRivers (Hydrology.hpp RiverWidthLaw, on the node's qArea) so
// the imprinted channel and the promoted ribbon agree.
struct RiverWidthLaw;
void imprintMasterChannels(const GridSpec& sim, MacroResult& macro,
                           vector<f32>& extraKeep,
                           const ProceduralControls& controls,
                           const MacroParams& macroParams,
                           const MasterNetworkParams& network,
                           const MasterImprintParams& imprint,
                           const RiverWidthLaw& law);

} // namespace render::terraingen
