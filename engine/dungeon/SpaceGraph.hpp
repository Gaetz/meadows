#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/dungeon/MissionGraph.hpp"

// Stage D2 of the dungeon pipeline (docs/DUNGEON-GEN.md) — the space graph:
// the mission graph laid out on a coarse grid of node slots, X x Z x floors.
// Starting from a grid (the Unexplored trick) sidesteps planar embedding:
// per-floor planarity holds by construction, the floor axis is free.
// Verticality lives here: rooms carry a floor INTERVAL (tall rooms, central
// shafts), edges carry a traversal type — ramps are walkable both ways,
// vertical drops are the natural one-way edge.
//
// Tunnels are routed through FREE grid cells (BFS) and reserve them, so a
// corridor never pierces an unrelated room — that is what keeps the D1 lock
// topology intact through the embedding (re-asserted by isSolvable below).

namespace dungeon {

struct GridPos {
    i32 x { 0 };
    i32 z { 0 };
    i32 floor { 0 };

    bool operator==(const GridPos&) const = default;
};

struct SpaceRoom {
    u32 missionNode { 0 }; // back-reference into stage D1
    GridPos pos;
    i32 floorSpan { 1 };   // slots occupied upward from pos.floor (tall rooms)
    f32 radius { 4.0f };
};

// A traversal between two rooms, as the polyline of grid cells the tunnel
// was routed through (endpoints included). Segment slopes decide walkability:
// XZ moves are flat, diagonal floor moves are ramps, pure-vertical moves are
// shafts (only produced for one-way drops).
struct SpaceEdge {
    u32 a { 0 };
    u32 b { 0 };
    EdgeKind kind { EdgeKind::Passage }; // carried from D1 (hidden, locked...)
    bool oneWay { false };
    u32 lockId { 0 };
    vector<GridPos> path;
};

struct SpaceParams {
    u32 seed { 1337 };
    // Rooms live on the interior even sub-lattice: usable slots per floor
    // are ((grid - 4) / 2 + 1)^2 — a 6-grid holds ONE, an 8-grid four.
    i32 gridX { 8 };
    i32 gridZ { 8 };
    i32 floors { 2 };
    f32 cellSpacing { 14.0f };  // meters between slot centers, XZ
    f32 floorSpacing { 11.0f }; // meters between floors (floor i at -i * this)
    f32 roomRadiusMin { 6.0f };
    f32 roomRadiusMax { 9.0f };
    f64 tallRoomChance { 0.25 }; // chance a room spans one extra floor
    // Smooth per-slot height offset within a floor (a natural mine is
    // never level): rooms tilt gently against their neighbours and the
    // corridors joining them turn into soft slopes. Keep well under the
    // nav budget left by the ramps.
    f32 slotHeightJitter { 1.5f };
    i32 attempts { 96 };         // embedding retries before giving up
};

struct SpaceGraph {
    vector<SpaceRoom> rooms;
    vector<SpaceEdge> edges;
    u32 entrance { 0 };
    u32 goal { 0 };
    SpaceParams params; // carried so later stages derive world positions
};

// Deterministic for (mission, params). Returns an empty graph (no rooms) if
// no attempt fits — callers treat that as "retune params", never as a crash.
SpaceGraph buildSpaceGraph(const MissionGraph& mission,
                           const SpaceParams& params);

// World-space center of a grid slot / a room (entrance floor is 0, floors
// stack DOWN: y = -floor * floorSpacing, absolute interior coordinates).
Vec3 slotCenter(const SpaceParams& params, const GridPos& pos);
Vec3 roomCenter(const SpaceGraph& graph, u32 room);

// The D1 invariant re-asserted after embedding (locks, one-ways, keys).
bool isSolvable(const MissionGraph& mission, const SpaceGraph& graph);

// Top view, one map per floor — the debug view of stage D2.
str toAscii(const SpaceGraph& graph);

} // namespace dungeon
