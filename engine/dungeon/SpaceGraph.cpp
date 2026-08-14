#include "engine/dungeon/SpaceGraph.hpp"

#include "engine/core/Rng.hpp"
#include "engine/terrain/Noise.hpp"

#include <algorithm>
#include <string>

namespace dungeon {

namespace {

constexpr i32 kFree = -1;
// Plain two-way unlocked corridors may cross each other (tunnels meeting is
// natural in a mine and cannot bypass a lock); locked, hidden and one-way
// corridors stay exclusive so the D1 topology survives the carve.
constexpr i32 kCorridorShared = -2;
constexpr i32 kCorridorExclusive = -3;

struct Grid {
    i32 sx { 0 }, sz { 0 }, sf { 0 };
    vector<i32> cells; // kFree, kCorridor*, or room index

    void init(const SpaceParams& p) {
        sx = p.gridX;
        sz = p.gridZ;
        sf = p.floors;
        cells.assign(static_cast<size_t>(sx * sz * sf), kFree);
    }
    bool inside(const GridPos& g) const {
        return g.x >= 0 && g.x < sx && g.z >= 0 && g.z < sz && g.floor >= 0 &&
               g.floor < sf;
    }
    i32& at(const GridPos& g) {
        return cells[static_cast<size_t>((g.floor * sz + g.z) * sx + g.x)];
    }
    i32 at(const GridPos& g) const {
        return cells[static_cast<size_t>((g.floor * sz + g.z) * sx + g.x)];
    }
};

// Rooms live on the even (x, z) sub-lattice — odd rows/columns stay free as
// corridor channels — and NEVER on the outermost ring: a border room only
// has 2-3 of the 4 channels a room's degree may need (the corner rooms were
// the unroutable seeds). Capacity lost to the margin comes back through the
// grid growth in buildSpaceGraph.
bool roomSlot(const Grid& grid, const GridPos& g) {
    return g.x % 2 == 0 && g.z % 2 == 0 && g.x >= 2 && g.z >= 2 &&
           g.x <= grid.sx - 3 && g.z <= grid.sz - 3;
}

// Deterministic expanding search: nearest free room slot to `anchor` from
// `minRing` outward, preferring `wantFloor`, scanning rings in a fixed
// order so the same seed replays. Later embedding attempts raise minRing:
// packed clusters starve corridor channels (ramp prisms included), and a
// bigger grid alone never loosens a nearest-slot cluster.
bool findFreeSlot(const Grid& grid, const GridPos& anchor, i32 wantFloor,
                  i32 minRing, GridPos& out) {
    for (i32 ring = minRing; ring < grid.sx + grid.sz; ++ring) {
        for (i32 df = 0; df < grid.sf; ++df) {
            // 0, -1, +1, -2, +2... around the wanted floor.
            const i32 floor =
                wantFloor + (df % 2 == 1 ? -(df + 1) / 2 : df / 2);
            if (floor < 0 || floor >= grid.sf) {
                continue;
            }
            for (i32 dz = -ring; dz <= ring; ++dz) {
                for (i32 dx = -ring; dx <= ring; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                        continue;
                    }
                    const GridPos g { anchor.x + dx, anchor.z + dz, floor };
                    if (grid.inside(g) && roomSlot(grid, g) &&
                        grid.at(g) == kFree) {
                        out = g;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// Routes a corridor from room `a` to room `b` through FREE cells only (so a
// tunnel never pierces an unrelated room — the lock topology survives).
// Moves: flat XZ steps, ramp steps (XZ + floor, walkable both ways), and —
// only for one-way edges — pure vertical drops. Plain BFS, deterministic
// neighbor order.
bool routeCorridor(Grid& grid, i32 roomA, i32 roomB, bool allowDrop,
                   bool shareable, vector<GridPos>& outPath) {
    constexpr i32 kUnvisited = -1;
    vector<i32> prev(grid.cells.size(), kUnvisited); // else parent flat index
    vector<GridPos> queue;
    const auto index = [&grid](const GridPos& g) {
        return (g.floor * grid.sz + g.z) * grid.sx + g.x;
    };
    for (i32 f = 0; f < grid.sf; ++f) {
        for (i32 z = 0; z < grid.sz; ++z) {
            for (i32 x = 0; x < grid.sx; ++x) {
                const GridPos g { x, z, f };
                if (grid.at(g) == roomA) {
                    queue.push_back(g);
                    prev[static_cast<size_t>(index(g))] = index(g);
                }
            }
        }
    }
    // Steps as {dx, dz, dfloor}, floor indices growing DOWNWARD: four flat
    // moves, eight ramps (XZ + one floor either way), one pure drop.
    static constexpr i32 kSteps[][3] = {
        { 1, 0, 0 },  { -1, 0, 0 },  { 0, 1, 0 },   { 0, -1, 0 },
        { 1, 0, 1 },  { -1, 0, 1 },  { 0, 1, 1 },   { 0, -1, 1 },
        { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, -1 },  { 0, -1, -1 },
        { 0, 0, 1 }, // pure drop, gated below
    };
    const auto traversable = [&](const GridPos& g) {
        if (!grid.inside(g)) {
            return false;
        }
        const i32 occupant = grid.at(g);
        return occupant == kFree || occupant == roomA || occupant == roomB ||
               (shareable && occupant == kCorridorShared);
    };
    // Floor-changing hops never share: their tube crosses the vertical
    // prism of both columns mid-air, so any other corridor through those
    // cells ends up pierced from above (a ramp mouth in its ceiling — an
    // unclimbable hole instead of a junction).
    const auto traversableStrict = [&](const GridPos& g) {
        if (!grid.inside(g)) {
            return false;
        }
        const i32 occupant = grid.at(g);
        return occupant == kFree || occupant == roomA || occupant == roomB;
    };
    GridPos found { -1, -1, -1 };
    for (size_t head = 0; head < queue.size() && found.x < 0; ++head) {
        const GridPos cur = queue[head];
        for (const auto& s : kSteps) {
            const bool drop = s[0] == 0 && s[1] == 0;
            if (drop && !allowDrop) {
                continue;
            }
            const GridPos next { cur.x + s[0], cur.z + s[1],
                                 cur.floor + s[2] };
            if (!grid.inside(next) ||
                prev[static_cast<size_t>(index(next))] != kUnvisited ||
                !(s[2] != 0 ? traversableStrict(next)
                            : traversable(next))) {
                continue;
            }
            if (s[2] != 0 && !drop) {
                // Ramps run between CHANNEL cells only: a big room's air
                // envelope swallows the end of a ramp landing on its slot
                // (the last stretch hovers over the room floor — an
                // unclimbable drop). Rooms are always entered flat, at
                // their own floor level. Vertical drops may still pierce
                // rooms: falling in is the one-way point.
                if (grid.at(cur) >= 0 || grid.at(next) >= 0) {
                    continue;
                }
                const GridPos prismA { cur.x, cur.z, next.floor };
                const GridPos prismB { next.x, next.z, cur.floor };
                if (!traversableStrict(prismA) ||
                    !traversableStrict(prismB)) {
                    continue;
                }
            }
            prev[static_cast<size_t>(index(next))] = index(cur);
            if (grid.at(next) == roomB) {
                found = next;
                break;
            }
            queue.push_back(next);
        }
    }
    if (found.x < 0) {
        return false;
    }
    // Unwind, then mark the free cells of the path as corridor.
    vector<GridPos> path;
    GridPos cur = found;
    while (true) {
        path.push_back(cur);
        const i32 p = prev[static_cast<size_t>(index(cur))];
        if (p == index(cur)) {
            break;
        }
        cur = { p % grid.sx, (p / grid.sx) % grid.sz,
                p / (grid.sx * grid.sz) };
    }
    std::reverse(path.begin(), path.end());
    // Self-crossing check on the FINAL path only (checking during the
    // search would sterilize it): no cell of this path may sit in the
    // vertical prism of one of its own ramp hops — the ramp would pierce
    // its own corridor's ceiling and turn the edge one-way in game (the
    // seed-1336 playtest trap). Rejecting sends the attempt to retry.
    for (size_t i = 1; i < path.size(); ++i) {
        const GridPos& a = path[i - 1];
        const GridPos& b = path[i];
        if (a.floor == b.floor || (a.x == b.x && a.z == b.z)) {
            continue;
        }
        const GridPos prisms[] = { { a.x, a.z, b.floor },
                                   { b.x, b.z, a.floor } };
        for (const GridPos& prism : prisms) {
            for (const GridPos& g : path) {
                if (g == prism) {
                    return false;
                }
            }
        }
    }
    const auto claim = [&](const GridPos& g, bool exclusive) {
        i32& cell = grid.at(g);
        if (cell == kFree ||
            (exclusive && cell == kCorridorShared)) {
            cell = exclusive || !shareable ? kCorridorExclusive
                                           : kCorridorShared;
        }
    };
    // Floor-changing hops first: their pair AND vertical prism go exclusive
    // even on a shareable edge (see traversableStrict). Flat cells then take
    // the edge's own class.
    for (size_t i = 1; i < path.size(); ++i) {
        const GridPos& a = path[i - 1];
        const GridPos& b = path[i];
        if (a.floor == b.floor) {
            continue;
        }
        claim(a, true);
        claim(b, true);
        if (a.x != b.x || a.z != b.z) {
            claim({ a.x, a.z, b.floor }, true);
            claim({ b.x, b.z, a.floor }, true);
        }
    }
    for (const GridPos& g : path) {
        claim(g, false);
    }
    outPath = std::move(path);
    return true;
}

bool tryEmbed(const MissionGraph& mission, const SpaceParams& params,
              u32 attempt, SpaceGraph& out) {
    core::Rng rng(params.seed + attempt * 7919u);
    Grid grid;
    grid.init(params);

    // Placement follows the mission graph breadth-first from the entrance,
    // each node landing near its already-placed parent.
    const u32 nodeCount = static_cast<u32>(mission.nodes.size());
    vector<i32> roomOf(nodeCount, -1);
    vector<u32> order;
    vector<u32> parent(nodeCount, 0);
    {
        vector<bool> seen(nodeCount, false);
        order.push_back(mission.entrance);
        seen[mission.entrance] = true;
        for (size_t head = 0; head < order.size(); ++head) {
            const u32 cur = order[head];
            for (const MissionEdge& e : mission.edges) {
                const u32 other = e.a == cur ? e.b : (e.b == cur ? e.a : cur);
                if (other != cur && !seen[other]) {
                    seen[other] = true;
                    parent[other] = cur;
                    order.push_back(other);
                }
            }
        }
        if (order.size() != nodeCount) {
            return false; // disconnected mission graph: nothing to embed
        }
    }

    out.rooms.clear();
    out.edges.clear();
    out.params = params;
    for (const u32 node : order) {
        GridPos slot;
        if (node == mission.entrance) {
            // Floor 0, one slot in from the border: a border room only has
            // three corridor channels, one short of the entrance's degree
            // (two cycle arcs + the service exit). The outside door is a
            // marker teleport, so nothing needs the grid edge itself.
            slot = { 2, (params.gridZ / 2) & ~1, 0 };
            if (grid.at(slot) != kFree) {
                return false;
            }
            // The border-side channel stays corridor-free: the exit door
            // to the overworld stands against that wall.
            grid.at({ slot.x - 1, slot.z, 0 }) = kCorridorExclusive;
        } else if (node == mission.goal) {
            // The goal anchors the FAR corner on the DEEPEST floor before
            // anything else clusters (the Unexplored order: pin start and
            // goal, then stretch the loop between them) — a mine digs
            // down toward its prize, and "the goal one corridor from the
            // door" was the first playtest complaint.
            const GridPos far { (params.gridX - 3) & ~1,
                                (params.gridZ / 2) & ~1,
                                params.floors - 1 };
            if (!findFreeSlot(grid, far, far.floor, 0, slot)) {
                return false;
            }
        } else {
            const SpaceRoom& near =
                out.rooms[static_cast<size_t>(roomOf[parent[node]])];
            i32 wantFloor = near.pos.floor;
            if (params.floors > 1 && rng.chance(0.35)) {
                wantFloor += rng.chance(0.7) ? 1 : -1; // bias downward: mines dig
                wantFloor = std::clamp(wantFloor, 0, params.floors - 1);
            }
            const i32 spread = std::min(3, static_cast<i32>(attempt) / 8);
            const i32 minRing = 1 + rng.range(0, spread);
            if (!findFreeSlot(grid, near.pos, wantFloor, minRing, slot)) {
                return false;
            }
        }
        SpaceRoom room;
        room.missionNode = node;
        room.pos = slot;
        room.radius = params.roomRadiusMin +
                      static_cast<f32>(rng.unit()) *
                          (params.roomRadiusMax - params.roomRadiusMin);
        room.floorSpan = 1;
        const GridPos above { slot.x, slot.z, slot.floor + 1 };
        if (grid.inside(above) && grid.at(above) == kFree &&
            rng.chance(params.tallRoomChance)) {
            room.floorSpan = 2;
        }
        const i32 roomIndex = static_cast<i32>(out.rooms.size());
        grid.at(slot) = roomIndex;
        if (room.floorSpan == 2) {
            grid.at(above) = roomIndex;
        }
        roomOf[node] = roomIndex;
        out.rooms.push_back(room);
    }

    for (const MissionEdge& e : mission.edges) {
        SpaceEdge edge;
        edge.a = static_cast<u32>(roomOf[e.a]);
        edge.b = static_cast<u32>(roomOf[e.b]);
        edge.kind = e.kind;
        edge.oneWay = e.oneWay;
        edge.lockId = e.lockId;
        const bool shareable =
            !e.oneWay && e.lockId == 0 &&
            (e.kind == EdgeKind::Passage || e.kind == EdgeKind::Dangerous);
        if (!routeCorridor(grid, static_cast<i32>(edge.a),
                           static_cast<i32>(edge.b), e.oneWay, shareable,
                           edge.path)) {
            return false;
        }
        out.edges.push_back(std::move(edge));
    }

    out.entrance = static_cast<u32>(roomOf[mission.entrance]);
    out.goal = static_cast<u32>(roomOf[mission.goal]);
    return true;
}

} // namespace

SpaceGraph buildSpaceGraph(const MissionGraph& mission,
                           const SpaceParams& params) {
    SpaceGraph graph;
    for (i32 attempt = 0; attempt < params.attempts; ++attempt) {
        // Congested missions get a growing grid every 8 failed attempts
        // (deterministic schedule): embedding then succeeds by construction
        // instead of failing seed by seed. graph.params carries the grid
        // actually used, so later stages follow.
        SpaceParams grown = params;
        grown.gridX += 2 * (attempt / 8);
        grown.gridZ += 2 * (attempt / 8);
        if (tryEmbed(mission, grown, static_cast<u32>(attempt), graph)) {
            return graph;
        }
    }
    graph = SpaceGraph {};
    graph.params = params;
    return graph; // empty: caller retunes (bigger grid, fewer rooms)
}

Vec3 slotCenter(const SpaceParams& params, const GridPos& pos) {
    // Low-frequency value noise: neighbouring slots stay correlated, so
    // the offsets read as gentle terrain, not as steps.
    const f32 jitter =
        params.slotHeightJitter *
        (2.0f * render::noise::value(params.seed,
                                     static_cast<f32>(pos.x) * 0.37f,
                                     static_cast<f32>(pos.z) * 0.37f) -
         1.0f);
    return { (static_cast<f32>(pos.x) + 0.5f) * params.cellSpacing,
             -static_cast<f32>(pos.floor) * params.floorSpacing + jitter,
             (static_cast<f32>(pos.z) + 0.5f) * params.cellSpacing };
}

Vec3 roomCenter(const SpaceGraph& graph, u32 room) {
    return slotCenter(graph.params, graph.rooms[room].pos);
}

bool isSolvable(const MissionGraph& mission, const SpaceGraph& graph) {
    if (graph.rooms.empty()) {
        return false;
    }
    // Project the embedding back onto a mission graph and reuse the D1 check:
    // same nodes semantics, edges as embedded.
    MissionGraph proxy;
    proxy.nodes.reserve(graph.rooms.size());
    for (const SpaceRoom& room : graph.rooms) {
        proxy.nodes.push_back(mission.nodes[room.missionNode]);
    }
    for (const SpaceEdge& e : graph.edges) {
        proxy.edges.push_back({ e.a, e.b, e.kind, e.oneWay, e.lockId });
    }
    proxy.entrance = graph.entrance;
    proxy.goal = graph.goal;
    return isSolvable(proxy);
}

str toAscii(const SpaceGraph& graph) {
    const SpaceParams& p = graph.params;
    str out;
    for (i32 f = 0; f < p.floors; ++f) {
        out += "floor " + std::to_string(f) + "\n";
        for (i32 z = 0; z < p.gridZ; ++z) {
            for (i32 x = 0; x < p.gridX; ++x) {
                char c = '.';
                for (const SpaceEdge& e : graph.edges) {
                    for (const GridPos& g : e.path) {
                        if (g.x == x && g.z == z && g.floor == f) {
                            c = '#';
                        }
                    }
                }
                for (u32 r = 0; r < graph.rooms.size(); ++r) {
                    const SpaceRoom& room = graph.rooms[r];
                    const bool covers =
                        room.pos.x == x && room.pos.z == z &&
                        f >= room.pos.floor &&
                        f < room.pos.floor + room.floorSpan;
                    if (!covers) {
                        continue;
                    }
                    if (r == graph.entrance) {
                        c = 'E';
                    } else if (r == graph.goal) {
                        c = 'G';
                    } else if (f > room.pos.floor) {
                        c = '|'; // upper part of a tall room
                    } else {
                        c = 'o';
                    }
                }
                out += c;
            }
            out += '\n';
        }
        out += '\n';
    }
    return out;
}

} // namespace dungeon
