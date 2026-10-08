#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"

// The MAP LATTICE as a world structure (docs/PAYSAGE.md, chantier
// CARTES): maps are mapSize squares on a grid; every grid line is a
// border transition — a mountain range (Ridges) or a sea arm (Sea),
// hashed per line segment. The style of a segment is a world-level
// fact read by TWO layers: the zones (a Ridges line crosses a rampart
// of cells that never sit below their neighbours, so the range rises
// out of a divide) and the border shaping of TerrainGen (the lift and
// the sea cut). Both read it HERE, so they never disagree.

namespace render::terraingen {

enum class MapEdgeStyle : i32 { Sea = 0, Ridges = 1 };

// A Sea proposal needs at least this fraction of its segment's samples
// under sea in the WORLD layer to stand; otherwise it demotes to
// Ridges (no 4 km canal dug across a continent).
constexpr f32 kMapBorderSeaVetoOceanFrac = 0.34f;

// The hashed style PROPOSAL of one border LINE segment: `lineIndex` is
// the grid index of the line (x = lineIndex * mapSize for vertical),
// `cellCross` the map coordinate along the crossing axis.
MapEdgeStyle mapBorderStyle(u32 seed, i32 lineIndex, i32 cellCross,
                            bool vertical);

// The RESOLVED style of a segment: the proposal with the Sea veto
// applied against the world layer sampled along the nominal line
// (memoized per segment, thread-local, keyed on every world param).
// Pure in (world, mapSize, segment): the zones, the bakes and the
// runtime fallback resolve identically.
MapEdgeStyle mapBorderSegmentStyle(const WorldLayerParams& world,
                                   f32 mapSize, i32 lineIndex,
                                   i32 cellCross, bool vertical);

} // namespace render::terraingen
