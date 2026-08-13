#include <doctest/doctest.h>

#include "engine/dungeon/MissionGraph.hpp"

using namespace dungeon;

namespace {

MissionParams paramsWith(u32 seed, CyclePattern pattern) {
    MissionParams p;
    p.seed = seed;
    p.patterns = { pattern };
    return p;
}

i32 countKind(const MissionGraph& g, NodeKind kind) {
    i32 n = 0;
    for (const MissionNode& node : g.nodes) {
        if (node.kind == kind) {
            ++n;
        }
    }
    return n;
}

} // namespace

TEST_CASE("dungeon mission: same seed reproduces the graph bit for bit (§8)") {
    MissionParams p;
    p.seed = 42;
    const MissionGraph a = buildMissionGraph(p);
    const MissionGraph b = buildMissionGraph(p);
    CHECK(toDot(a) == toDot(b));

    p.seed = 43;
    const MissionGraph c = buildMissionGraph(p);
    CHECK(toDot(a) != toDot(c));
}

TEST_CASE("dungeon mission: every pattern is solvable across many seeds") {
    const CyclePattern all[] = {
        CyclePattern::TwoAlternativePaths, CyclePattern::SimpleLockKey,
        CyclePattern::HiddenShortcut,      CyclePattern::DangerousRoute,
        CyclePattern::BlockedRetreat,
    };
    for (const CyclePattern pattern : all) {
        for (u32 seed = 1; seed <= 50; ++seed) {
            const MissionGraph g = buildMissionGraph(paramsWith(seed, pattern));
            CAPTURE(static_cast<int>(pattern));
            CAPTURE(seed);
            CHECK(isSolvable(g));
        }
    }
}

TEST_CASE("dungeon mission: mixed patterns with recursion stay solvable") {
    MissionParams p;
    p.subCycles = 3;
    p.maxDepth = 2;
    for (u32 seed = 1; seed <= 100; ++seed) {
        p.seed = seed;
        const MissionGraph g = buildMissionGraph(p);
        CAPTURE(seed);
        CHECK(isSolvable(g));
    }
}

TEST_CASE("dungeon mission: lock-and-key builds a locked edge with its key") {
    const MissionGraph g =
        buildMissionGraph(paramsWith(7, CyclePattern::SimpleLockKey));
    bool foundLocked = false;
    u32 lockId = 0;
    for (const MissionEdge& e : g.edges) {
        if (e.kind == EdgeKind::Locked) {
            foundLocked = true;
            lockId = e.lockId;
        }
    }
    REQUIRE(foundLocked);
    CHECK(lockId != 0);
    bool foundKey = false;
    for (const MissionNode& n : g.nodes) {
        if (n.kind == NodeKind::Key && n.lockId == lockId) {
            foundKey = true;
        }
    }
    CHECK(foundKey);
}

TEST_CASE("dungeon mission: sub-cycles graft rewards onto rooms") {
    MissionParams p;
    p.seed = 11;
    p.subCycles = 2;
    p.patterns = { CyclePattern::TwoAlternativePaths };
    const MissionGraph g = buildMissionGraph(p);
    CHECK(countKind(g, NodeKind::Reward) == 2);
    for (const MissionNode& n : g.nodes) {
        if (n.kind == NodeKind::Reward) {
            CHECK(n.depth == 1);
        }
    }
}

TEST_CASE("dungeon mission: solvability rejects a keyless lock") {
    MissionGraph g;
    g.nodes.push_back({ 0, NodeKind::Entrance, 0,
                        CyclePattern::TwoAlternativePaths });
    g.nodes.push_back({ 0, NodeKind::Goal, 0,
                        CyclePattern::TwoAlternativePaths });
    g.entrance = 0;
    g.goal = 1;
    g.edges.push_back({ 0, 1, EdgeKind::Locked, false, 1 });
    CHECK_FALSE(isSolvable(g));

    // Adding the key (reachable before the lock) repairs it.
    g.nodes.push_back({ 1, NodeKind::Key, 0,
                        CyclePattern::TwoAlternativePaths });
    g.edges.push_back({ 0, 2, EdgeKind::Passage, false, 0 });
    CHECK(isSolvable(g));
}

TEST_CASE("dungeon mission: solvability rejects a one-way trap with no exit") {
    MissionGraph g;
    g.nodes.push_back({ 0, NodeKind::Entrance, 0,
                        CyclePattern::BlockedRetreat });
    g.nodes.push_back({ 0, NodeKind::Goal, 0, CyclePattern::BlockedRetreat });
    g.entrance = 0;
    g.goal = 1;
    g.edges.push_back({ 0, 1, EdgeKind::Passage, true, 0 }); // drop, no return
    CHECK_FALSE(isSolvable(g));

    // A return arc restores the cycle promise.
    g.edges.push_back({ 1, 0, EdgeKind::Passage, true, 0 });
    CHECK(isSolvable(g));
}

TEST_CASE("dungeon mission: dot dump names entrance and goal") {
    const MissionGraph g = buildMissionGraph(
        paramsWith(3, CyclePattern::TwoAlternativePaths));
    const str dot = toDot(g);
    CHECK(dot.find("entrance") != str::npos);
    CHECK(dot.find("goal") != str::npos);
    CHECK(dot.find("digraph") != str::npos);
}
