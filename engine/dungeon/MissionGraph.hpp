#pragma once

#include "engine/core/Defines.hpp"

// Stage D1 of the dungeon pipeline (docs/DUNGEON-GEN.md) — the mission graph:
// the dungeon as gameplay intent, before any spatial layout. The design unit
// is the CYCLE (Dormans / Unexplored): two differentiated arcs between an
// entrance and a goal, recursively nestable at any Room node. Lock<->key
// relations are edges/fields of the graph itself and must survive every later
// stage: solvability is a structural invariant re-checked after each
// transformation, never a post-hoc repair.

namespace dungeon {

// Non-terminal node roles, resolved into concrete content by later stages
// ("low resolution first"): a Key may become a lever, an item, or knowledge;
// locks live on edges, not nodes.
enum class NodeKind : u8 {
    Entrance,
    Goal,
    Room,   // neutral filler; the insertion point for sub-cycles
    Key,    // grants its lockId when visited
    Reward, // treasure / ore vein / chest (also the local goal of sub-cycles)
};

enum class EdgeKind : u8 {
    Passage,   // plain traversal
    Locked,    // requires the key holding the matching lockId
    Hidden,    // secret passage (findable, so traversable for solvability)
    Dangerous, // hazard arc: steep drop, unstable gallery, ambush
};

// Cycle vocabulary (the ~12 Unexplored patterns); v1 builds the subset mines
// need. The enum keeps names ahead of implementation so params/data can quote
// a pattern before its build function exists (adding one = one function).
enum class CyclePattern : u8 {
    TwoAlternativePaths,
    SimpleLockKey,
    HiddenShortcut,
    DangerousRoute,
    BlockedRetreat,
};

struct MissionNode {
    u32 lockId { 0 }; // Key nodes: the lock this opens (0 = none)
    NodeKind kind { NodeKind::Room };
    u8 depth { 0 };   // cycle nesting depth (0 = main cycle)
    CyclePattern pattern { CyclePattern::TwoAlternativePaths }; // creator
};

struct MissionEdge {
    u32 a { 0 };
    u32 b { 0 };
    EdgeKind kind { EdgeKind::Passage };
    bool oneWay { false }; // traversable a->b only (collapse, chute)
    u32 lockId { 0 };      // Locked edges: key required
};

struct MissionGraph {
    vector<MissionNode> nodes;
    vector<MissionEdge> edges;
    u32 entrance { 0 };
    u32 goal { 0 };
};

// The design dial (§ "algorithme à tiroirs"): a mine and a temple differ by
// these values, not by code. Patterns absent from `patterns` are never drawn.
struct MissionParams {
    u32 seed { 1337 };
    vector<CyclePattern> patterns {
        CyclePattern::TwoAlternativePaths, CyclePattern::SimpleLockKey,
        CyclePattern::HiddenShortcut,      CyclePattern::DangerousRoute,
        CyclePattern::BlockedRetreat,
    };
    i32 subCycles { 1 }; // sub-cycles grafted onto Room nodes
    i32 maxDepth { 1 };  // nesting depth allowed for those grafts
    i32 arcRoomsMin { 1 };
    i32 arcRoomsMax { 3 };
};

// Deterministic for (params): same seed -> same graph, bit for bit.
MissionGraph buildMissionGraph(const MissionParams& params);

// Reachability with key collection, honouring one-way and locked edges:
// true iff every node is reachable from the entrance AND the entrance is
// reachable back from the goal (the player can always get out — the cycle's
// promise). This is the invariant later stages re-assert after embedding.
bool isSolvable(const MissionGraph& graph);

// Graphviz dump (`dot -Tpng`) — the debug view of stage D1.
str toDot(const MissionGraph& graph);

} // namespace dungeon
