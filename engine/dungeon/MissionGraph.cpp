#include "engine/dungeon/MissionGraph.hpp"

#include "engine/core/Rng.hpp"

#include <set>
#include <string>
#include <utility>

namespace dungeon {

namespace {

u32 addNode(MissionGraph& g, NodeKind kind, u8 depth, CyclePattern pattern,
            u32 lockId = 0) {
    g.nodes.push_back({ lockId, kind, depth, pattern });
    return static_cast<u32>(g.nodes.size() - 1);
}

void addEdge(MissionGraph& g, u32 a, u32 b, EdgeKind kind = EdgeKind::Passage,
             bool oneWay = false, u32 lockId = 0) {
    g.edges.push_back({ a, b, kind, oneWay, lockId });
}

// A chain of filler rooms from -> r1 -> ... -> rn -> to. Every edge carries
// `kind`; `oneWayFirst` puts a one-way on the FIRST hop (the collapse of
// BlockedRetreat happens right behind the player).
void addArc(MissionGraph& g, u32 from, u32 to, i32 rooms, u8 depth,
            CyclePattern pattern, EdgeKind kind = EdgeKind::Passage,
            bool oneWayFirst = false) {
    u32 prev = from;
    for (i32 i = 0; i < rooms; ++i) {
        const u32 room = addNode(g, NodeKind::Room, depth, pattern);
        addEdge(g, prev, room, kind, oneWayFirst && i == 0);
        prev = room;
    }
    addEdge(g, prev, to, kind, oneWayFirst && rooms == 0);
}

struct BuildContext {
    core::Rng rng;
    u32 nextLockId { 0 };
};

i32 arcRooms(BuildContext& ctx, const MissionParams& p) {
    return ctx.rng.range(p.arcRoomsMin, p.arcRoomsMax);
}

// Builds one cycle of `pattern` between two EXISTING nodes. Every pattern
// keeps the cycle promise: two arcs, `from` and `goal` on both, so the player
// can always come back — which is what isSolvable()'s exit check verifies.
// Arc lengths come straight from the params: travel distance is already
// guaranteed by the far-goal anchoring, so topology stays as simple as the
// dial asks (playtest: padded arcs read as complexity, not as length).
void buildCycle(MissionGraph& g, BuildContext& ctx, const MissionParams& p,
                u32 from, u32 goal, CyclePattern pattern, u8 depth) {
    const i32 shortArc = p.arcRoomsMin;
    switch (pattern) {
    case CyclePattern::TwoAlternativePaths: {
        // Short arc vs long arc, both plain: the minimal player choice.
        addArc(g, from, goal, shortArc, depth, pattern);
        addArc(g, from, goal, arcRooms(ctx, p), depth, pattern);
        break;
    }
    case CyclePattern::SimpleLockKey: {
        // The signature anti-backtracking cycle: the goal sits solely behind
        // the lock; the long arc ends on the key, one step from the lock room
        // (see the lock early, open it right after finding the key).
        const u32 lockId = ++ctx.nextLockId;
        const u32 lockRoom = addNode(g, NodeKind::Room, depth, pattern);
        addArc(g, from, lockRoom, shortArc, depth, pattern);
        addEdge(g, lockRoom, goal, EdgeKind::Locked, false, lockId);
        const u32 key = addNode(g, NodeKind::Key, depth, pattern, lockId);
        addArc(g, from, key, arcRooms(ctx, p), depth, pattern);
        addEdge(g, key, lockRoom); // the shortcut back to the lock
        break;
    }
    case CyclePattern::HiddenShortcut: {
        // Long visible route; a short secret path rewards search and speeds
        // up replays.
        addArc(g, from, goal, arcRooms(ctx, p) + 1, depth, pattern);
        addArc(g, from, goal, 1, depth, pattern, EdgeKind::Hidden);
        break;
    }
    case CyclePattern::DangerousRoute: {
        // Short but hazardous vs long but safe: risk/time trade-off.
        addArc(g, from, goal, shortArc, depth, pattern,
               EdgeKind::Dangerous);
        addArc(g, from, goal, arcRooms(ctx, p) + 1, depth, pattern);
        break;
    }
    case CyclePattern::BlockedRetreat: {
        // The way in collapses behind the player (one-way on the first hop);
        // the second arc is the guaranteed way back out.
        addArc(g, from, goal, arcRooms(ctx, p), depth, pattern,
               EdgeKind::Passage, true);
        addArc(g, goal, from, arcRooms(ctx, p), depth, pattern);
        break;
    }
    }
}

CyclePattern drawPattern(BuildContext& ctx, const MissionParams& p) {
    if (p.patterns.empty()) {
        return CyclePattern::TwoAlternativePaths;
    }
    const i32 i = ctx.rng.range(0, static_cast<i32>(p.patterns.size()) - 1);
    return p.patterns[static_cast<size_t>(i)];
}

const char* nodeLabel(NodeKind k) {
    switch (k) {
    case NodeKind::Entrance: return "entrance";
    case NodeKind::Goal: return "goal";
    case NodeKind::Room: return "room";
    case NodeKind::Key: return "key";
    case NodeKind::Reward: return "reward";
    }
    return "?";
}

} // namespace

MissionGraph buildMissionGraph(const MissionParams& params) {
    MissionGraph g;
    BuildContext ctx { core::Rng { params.seed }, 0 };

    g.entrance = addNode(g, NodeKind::Entrance, 0,
                         CyclePattern::TwoAlternativePaths);
    g.goal = addNode(g, NodeKind::Goal, 0, CyclePattern::TwoAlternativePaths);
    buildCycle(g, ctx, params, g.entrance, g.goal, drawPattern(ctx, params), 0);

    // Graft sub-cycles onto Room nodes: the room becomes the local entrance
    // of a nested cycle whose local goal is a Reward (an ore vein, a stash).
    // Hosts are capped at degree 2: a grafted cycle adds two arcs, and the
    // 4-neighbour corridor channels of the space grid cannot serve a room
    // of degree > 4 at ANY grid size (embedding would never converge).
    for (i32 i = 0; i < params.subCycles; ++i) {
        vector<u32> degree(g.nodes.size(), 0);
        for (const MissionEdge& e : g.edges) {
            ++degree[e.a];
            ++degree[e.b];
        }
        vector<u32> candidates;
        for (u32 n = 0; n < g.nodes.size(); ++n) {
            if (g.nodes[n].kind == NodeKind::Room &&
                g.nodes[n].depth < params.maxDepth && degree[n] <= 2) {
                candidates.push_back(n);
            }
        }
        if (candidates.empty()) {
            break;
        }
        const u32 host = candidates[static_cast<size_t>(
            ctx.rng.range(0, static_cast<i32>(candidates.size()) - 1))];
        const u8 depth = static_cast<u8>(g.nodes[host].depth + 1);
        const CyclePattern pattern = drawPattern(ctx, params);
        // Lollipop graft: one connecting corridor to a NEW hub, the cycle
        // hangs off the hub. The host gains a single exit (degree <= 3),
        // the hub peaks at 3 — junctions read "this corridor leads
        // somewhere", never as four-way crossroads (Unexplored's
        // coherence note, echoed by the playtest).
        const u32 hub = addNode(g, NodeKind::Room, depth, pattern);
        addEdge(g, host, hub);
        const u32 reward = addNode(g, NodeKind::Reward, depth, pattern);
        buildCycle(g, ctx, params, hub, reward, pattern, depth);
    }

    // The service exit: goal -> key room -> locked corridor -> entrance.
    // The lever (Key) is only reachable through the goal, so the shortcut
    // opens from behind; the barrier shows from the entrance side.
    if (params.serviceExit) {
        const u32 lockId = ++ctx.nextLockId;
        const u32 exitKey = addNode(g, NodeKind::Key, 0,
                                    CyclePattern::SimpleLockKey, lockId);
        addEdge(g, g.goal, exitKey);
        addEdge(g, exitKey, g.entrance, EdgeKind::Locked, false, lockId);
    }
    return g;
}

bool isSolvable(const MissionGraph& g) {
    if (g.nodes.empty()) {
        return false;
    }
    // Keys as a bitmask (lock ids are sequential and few).
    u32 maxLock = 0;
    for (const MissionEdge& e : g.edges) {
        maxLock = e.lockId > maxLock ? e.lockId : maxLock;
    }
    if (maxLock >= 63) {
        return false; // beyond any sane dungeon; refuse loudly
    }
    const auto keyBit = [&g](u32 node) -> u64 {
        return g.nodes[node].kind == NodeKind::Key
                   ? 1ull << g.nodes[node].lockId
                   : 0ull;
    };

    // Reachable set from (start, keys), collecting keys en route.
    const auto explore = [&g, &keyBit](u32 start, u64 startKeys) {
        vector<bool> in(g.nodes.size(), false);
        u64 keys = startKeys | keyBit(start);
        in[start] = true;
        bool grew = true;
        while (grew) {
            grew = false;
            for (const MissionEdge& e : g.edges) {
                if (e.kind == EdgeKind::Locked &&
                    (keys & (1ull << e.lockId)) == 0) {
                    continue;
                }
                const auto visit = [&](u32 n) {
                    if (!in[n]) {
                        in[n] = true;
                        keys |= keyBit(n);
                        grew = true;
                    }
                };
                if (in[e.a]) {
                    visit(e.b);
                }
                if (!e.oneWay && in[e.b]) {
                    visit(e.a);
                }
            }
        }
        return in;
    };

    // Everything must be reachable from the entrance.
    const vector<bool> fromEntrance = explore(g.entrance, 0);
    for (u32 n = 0; n < g.nodes.size(); ++n) {
        if (!fromEntrance[n]) {
            return false;
        }
    }

    // No-stranding over PLAY STATES (node, keys held): a player behind a
    // lock necessarily holds its key, and a player dropped by a one-way
    // may hold nothing — both must always have a way back to the
    // entrance. Key sets grow monotonically, so the state space is tiny.
    std::set<std::pair<u32, u64>> seen;
    vector<std::pair<u32, u64>> queue;
    const auto push = [&](u32 node, u64 keys) {
        keys |= keyBit(node);
        if (seen.insert({ node, keys }).second) {
            queue.push_back({ node, keys });
        }
    };
    push(g.entrance, 0);
    for (size_t head = 0; head < queue.size(); ++head) {
        const auto [node, keys] = queue[head];
        for (const MissionEdge& e : g.edges) {
            if (e.kind == EdgeKind::Locked &&
                (keys & (1ull << e.lockId)) == 0) {
                continue;
            }
            if (e.a == node) {
                push(e.b, keys);
            }
            if (e.b == node && !e.oneWay) {
                push(e.a, keys);
            }
        }
    }
    for (const auto& [node, keys] : seen) {
        if (!explore(node, keys)[g.entrance]) {
            return false;
        }
    }
    return true;
}

str toDot(const MissionGraph& g) {
    str out = "digraph mission {\n  rankdir=LR;\n";
    for (u32 n = 0; n < g.nodes.size(); ++n) {
        const MissionNode& node = g.nodes[n];
        out += "  n" + std::to_string(n) + " [label=\"" +
               nodeLabel(node.kind) + " " + std::to_string(n);
        if (node.lockId != 0) {
            out += " (lock " + std::to_string(node.lockId) + ")";
        }
        out += "\"";
        if (node.kind == NodeKind::Entrance || node.kind == NodeKind::Goal) {
            out += " shape=doublecircle";
        } else if (node.kind == NodeKind::Key ||
                   node.kind == NodeKind::Reward) {
            out += " shape=diamond";
        }
        out += "];\n";
    }
    for (const MissionEdge& e : g.edges) {
        out += "  n" + std::to_string(e.a) + " -> n" + std::to_string(e.b);
        str attrs;
        if (!e.oneWay) {
            attrs += " dir=both";
        }
        switch (e.kind) {
        case EdgeKind::Passage: break;
        case EdgeKind::Locked:
            attrs += " color=red label=\"lock " + std::to_string(e.lockId) +
                     "\"";
            break;
        case EdgeKind::Hidden: attrs += " style=dashed"; break;
        case EdgeKind::Dangerous: attrs += " color=orange"; break;
        }
        if (!attrs.empty()) {
            out += " [" + attrs.substr(1) + "]";
        }
        out += ";\n";
    }
    out += "}\n";
    return out;
}

} // namespace dungeon
