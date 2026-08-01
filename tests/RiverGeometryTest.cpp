#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/RiverGeometry.hpp"

// Adaptive river-polyline subdivision: straight reaches untouched, tight
// bends densified under the angle bound, hairpin half-widths clamped so
// the inner bank never folds.

using render::RiverNode;
using render::terrain::subdivideRiverNodes;

namespace {

f32 turnAt(const vector<RiverNode>& nodes, size_t i) {
    const f32 ax = nodes[i].x - nodes[i - 1].x;
    const f32 az = nodes[i].z - nodes[i - 1].z;
    const f32 bx = nodes[i + 1].x - nodes[i].x;
    const f32 bz = nodes[i + 1].z - nodes[i].z;
    return std::abs(std::atan2(ax * bz - az * bx, ax * bx + az * bz));
}

} // namespace

TEST_CASE("a straight polyline comes back unchanged") {
    vector<RiverNode> straight;
    for (int i = 0; i < 6; ++i) {
        straight.push_back({ static_cast<f32>(i) * 20.0f, 0.0f,
                             50.0f - static_cast<f32>(i), 3.0f });
    }
    const auto out = subdivideRiverNodes(straight);
    REQUIRE(out.size() == straight.size());
    for (size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].x == straight[i].x);
        CHECK(out[i].halfWidth == straight[i].halfWidth);
    }
}

TEST_CASE("a sharp bend densifies and respects the bounds") {
    // 90-degree corner over two 30 m segments.
    const vector<RiverNode> corner = {
        { 0.0f, 0.0f, 20.0f, 3.0f },
        { 30.0f, 0.0f, 19.0f, 3.0f },
        { 30.0f, 30.0f, 18.0f, 3.0f },
        { 30.0f, 60.0f, 17.0f, 3.0f },
    };
    const auto out = subdivideRiverNodes(corner, 0.14f, 2.0f, 8);
    CHECK(out.size() > corner.size());
    // Endpoints and order preserved.
    CHECK(out.front().x == corner.front().x);
    CHECK(out.back().z == corner.back().z);
    // The surface stays monotone downhill.
    for (size_t i = 1; i < out.size(); ++i) {
        CHECK(out[i].surface <= out[i - 1].surface + 1.0e-4f);
    }
    // Every interior emitted segment stays under a relaxed angle bound
    // (the curve smooths the corner; residual turns shrink with the
    // subdivision).
    f32 maxTurn = 0.0f;
    for (size_t i = 1; i + 1 < out.size(); ++i) {
        maxTurn = std::max(maxTurn, turnAt(out, i));
    }
    f32 rawTurn = 0.0f;
    for (size_t i = 1; i + 1 < corner.size(); ++i) {
        rawTurn = std::max(rawTurn, turnAt(corner, i));
    }
    CHECK(maxTurn < rawTurn * 0.6f);
    // minStep respected.
    for (size_t i = 1; i < out.size(); ++i) {
        const f32 step = std::hypot(out[i].x - out[i - 1].x,
                                    out[i].z - out[i - 1].z);
        CHECK(step > 1.5f);
    }
}

TEST_CASE("hairpin half-widths clamp under the turn radius") {
    // A 8 m-wide river through a hairpin with ~6 m segments: unclamped,
    // the inner bank would fold across the apex.
    const vector<RiverNode> hairpin = {
        { 0.0f, 0.0f, 10.0f, 8.0f },
        { 6.0f, 0.0f, 9.8f, 8.0f },
        { 9.0f, 5.0f, 9.6f, 8.0f },
        { 6.0f, 10.0f, 9.4f, 8.0f },
        { 0.0f, 10.0f, 9.2f, 8.0f },
    };
    const auto out = subdivideRiverNodes(hairpin);
    f32 minHalf = 1.0e9f;
    for (const RiverNode& node : out) {
        minHalf = std::min(minHalf, node.halfWidth);
    }
    CHECK(minHalf < 8.0f); // the apex narrowed
    CHECK(minHalf > 0.5f); // but stays a river
}
