#include <doctest/doctest.h>

#include "data/editor/GraphLayout.hpp"

// The deterministic layered auto-layout behind the graph
// editors. Pure data (guids in, positions out): what the canvas shows for
// a graph with no stored positions must never depend on hash order.

using core::Guid;

namespace {

Guid guid(u32 n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "dddd%04x-0000-4000-8000-000000000001", n);
    return *Guid::fromString(buf);
}

} // namespace

TEST_CASE("graph layout: layers follow BFS depth, rows follow guid order") {
    // root -> a, root -> b, a -> c (a classic fan-out).
    const Guid root = guid(1), a = guid(2), b = guid(3), c = guid(4);
    const vector<Guid> nodes { c, b, a, root }; // scrambled on purpose
    const vector<std::pair<Guid, Guid>> edges { { root, a },
                                                { root, b },
                                                { a, c } };
    const auto result = data::layoutGraph(nodes, edges, { root });
    REQUIRE(result.positions.size() == 4);

    const Vec2 pRoot = result.positions.at(root);
    const Vec2 pa = result.positions.at(a);
    const Vec2 pb = result.positions.at(b);
    const Vec2 pc = result.positions.at(c);
    CHECK(pRoot.x == doctest::Approx(0.0f));
    CHECK(pa.x > pRoot.x);      // depth 1
    CHECK(pb.x == doctest::Approx(pa.x));
    CHECK(pc.x > pa.x);         // depth 2
    CHECK(pa.y != pb.y);        // same layer, distinct rows
    // guid(2) < guid(3): a ranks above b, deterministically.
    CHECK(pa.y < pb.y);
}

TEST_CASE("graph layout: identical inputs give identical outputs") {
    const Guid root = guid(1), a = guid(2), b = guid(3);
    const vector<Guid> nodes { a, root, b };
    const vector<std::pair<Guid, Guid>> edges { { root, a }, { root, b } };
    const auto first = data::layoutGraph(nodes, edges, { root });
    const auto second = data::layoutGraph(nodes, edges, { root });
    for (const auto& [node, position] : first.positions) {
        CHECK(second.positions.at(node) == position);
    }
}

TEST_CASE("graph layout: a diamond keeps one node per (layer, row) slot") {
    // root -> a, root -> b, a -> d, b -> d: d sits at depth 2, once.
    const Guid root = guid(1), a = guid(2), b = guid(3), d = guid(4);
    const vector<Guid> nodes { root, a, b, d };
    const vector<std::pair<Guid, Guid>> edges {
        { root, a }, { root, b }, { a, d }, { b, d }
    };
    const auto result = data::layoutGraph(nodes, edges, { root });
    CHECK(result.positions.at(d).x > result.positions.at(a).x);
    // No two nodes share a position.
    for (const auto& [n1, p1] : result.positions) {
        for (const auto& [n2, p2] : result.positions) {
            if (n1 != n2) {
                CHECK(p1 != p2);
            }
        }
    }
}

TEST_CASE("graph layout: cycles terminate and place every node") {
    // a -> b -> c -> a (pure cycle, no root candidate at all).
    const Guid a = guid(1), b = guid(2), c = guid(3);
    const vector<Guid> nodes { a, b, c };
    const vector<std::pair<Guid, Guid>> edges { { a, b },
                                                { b, c },
                                                { c, a } };
    const auto result = data::layoutGraph(nodes, edges, {});
    CHECK(result.positions.size() == 3); // nobody lost, no hang
}

TEST_CASE("graph layout: orphans land in the final layer") {
    const Guid root = guid(1), a = guid(2), lost = guid(3);
    const vector<Guid> nodes { root, a, lost };
    const vector<std::pair<Guid, Guid>> edges { { root, a } };
    const auto result = data::layoutGraph(nodes, edges, { root });
    CHECK(result.positions.at(lost).x > result.positions.at(a).x);
}

// The re-parent anti-cycle guard of the dialogue graph.
TEST_CASE("isAncestorOf walks the parent chain, survives corrupt cycles") {
    const Guid root = guid(1), a = guid(2), b = guid(3), other = guid(4);
    // root <- a <- b (childToParent map).
    std::unordered_map<Guid, Guid> parentOf { { a, root }, { b, a } };

    CHECK(data::isAncestorOf(parentOf, root, b));  // grandparent
    CHECK(data::isAncestorOf(parentOf, a, b));     // parent
    CHECK(data::isAncestorOf(parentOf, b, b));     // self counts
    CHECK_FALSE(data::isAncestorOf(parentOf, b, a));     // child is not
    CHECK_FALSE(data::isAncestorOf(parentOf, other, b)); // stranger

    // The re-parent rule it implements: b may move under `other`
    // (other is no descendant of b), but root may NOT move under b.
    CHECK_FALSE(data::isAncestorOf(parentOf, other, b));
    CHECK(data::isAncestorOf(parentOf, root, b)); // would cycle -> refuse

    // A corrupt cycle in the map terminates instead of hanging.
    std::unordered_map<Guid, Guid> cyclic { { a, b }, { b, a } };
    CHECK_FALSE(data::isAncestorOf(cyclic, root, a));
}

TEST_CASE("graph layout: the rank-order key drives rows within a layer") {
    // Dialogue-tree usage: siblings ordered by `order`, not guid.
    const Guid root = guid(1), a = guid(2), b = guid(3);
    const vector<Guid> nodes { root, a, b };
    const vector<std::pair<Guid, Guid>> edges { { root, a }, { root, b } };
    const std::unordered_map<Guid, i32> order { { a, 5 }, { b, 1 } };
    const auto result = data::layoutGraph(nodes, edges, { root }, &order);
    // b (order 1) ranks above a (order 5) despite guid(2) < guid(3).
    CHECK(result.positions.at(b).y < result.positions.at(a).y);
}
