#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/generation/MasterNetwork.hpp"

// Stage 0 — the regional master hydrology (fleuve courses with TRUE
// drainage areas, routed on the analytic macro). Purity and the
// structural contracts; the seed-1337 look lives in the hidden
// diagnostic below.

using namespace render::terraingen;

TEST_CASE("master network: deterministic, monotone, owned by core") {
    ProceduralControlParams controlParams;
    controlParams.seed = 1337;
    const ProceduralControls controls { controlParams };
    const MacroParams macro;
    MasterNetworkParams params;

    const MasterNetwork a =
        computeMasterNetwork(controls, macro, params, 0, 0);
    const MasterNetwork b =
        computeMasterNetwork(controls, macro, params, 0, 0);
    REQUIRE(a.rivers.size() == b.rivers.size());
    for (size_t r = 0; r < a.rivers.size(); ++r) {
        REQUIRE(a.rivers[r].nodes.size() == b.rivers[r].nodes.size());
        for (size_t k = 0; k < a.rivers[r].nodes.size(); ++k) {
            CHECK(a.rivers[r].nodes[k].x == b.rivers[r].nodes[k].x);
            CHECK(a.rivers[r].nodes[k].surface ==
                  b.rivers[r].nodes[k].surface);
        }
    }

    const f32 coreMin = -params.apron + params.apron; // 0
    for (const MasterRiver& river : a.rivers) {
        REQUIRE(river.nodes.size() >= 4);
        // The head belongs to this super cell's core.
        CHECK(river.nodes.front().x >= coreMin);
        CHECK(river.nodes.front().x <= params.superRegionSize);
        CHECK(river.nodes.front().z >= coreMin);
        CHECK(river.nodes.front().z <= params.superRegionSize);
        // Downstream: surface monotone (non-increasing), area at the
        // fleuve threshold everywhere.
        for (size_t k = 1; k < river.nodes.size(); ++k) {
            CHECK(river.nodes[k].surface <=
                  river.nodes[k - 1].surface + 1.0e-3f);
        }
        for (const MasterNode& node : river.nodes) {
            CHECK(node.area >= params.fleuveArea);
        }
    }
}

TEST_CASE("master network: two callers agree through masterRiversNear") {
    ProceduralControlParams controlParams;
    controlParams.seed = 1337;
    const ProceduralControls controls { controlParams };
    const MacroParams macro;
    MasterNetworkParams params;

    // Two different query rects over the same territory: any course
    // present in both answers is bit-identical (ownership by head).
    const auto a = masterRiversNear(controls, macro, params, 2000.0f,
                                    2000.0f, 12000.0f, 12000.0f);
    const auto b = masterRiversNear(controls, macro, params, -4000.0f,
                                    -4000.0f, 20000.0f, 20000.0f);
    for (const MasterRiver& ra : a) {
        for (const MasterRiver& rb : b) {
            // Identity = head AND second node (grid-aligned head
            // coordinates can collide across super cells; two rivers
            // sharing two consecutive nodes would have merged).
            if (ra.nodes.front().x == rb.nodes.front().x &&
                ra.nodes.front().z == rb.nodes.front().z &&
                ra.nodes[1].x == rb.nodes[1].x &&
                ra.nodes[1].z == rb.nodes[1].z) {
                if (ra.nodes.size() != rb.nodes.size()) {
                    MESSAGE("mismatch: head (", ra.nodes.front().x,
                            ", ", ra.nodes.front().z, ") sizes ",
                            ra.nodes.size(), " vs ", rb.nodes.size(),
                            " mouths (", ra.nodes.back().x, ", ",
                            ra.nodes.back().z, ") vs (",
                            rb.nodes.back().x, ", ",
                            rb.nodes.back().z, ")");
                }
                REQUIRE(ra.nodes.size() == rb.nodes.size());
                CHECK(ra.nodes.back().x == rb.nodes.back().x);
                CHECK(ra.nodes.back().surface ==
                      rb.nodes.back().surface);
            }
        }
    }
    CHECK(true);
}

// Seed-1337 look of the master network around the spawn: how many
// fleuves, how far apart, do they align with the B4 trunk corridors
// and reach the sea.
//   meadows-tests '-tc=master network diagnostic' -ns
TEST_CASE("master network diagnostic" * doctest::skip()) {
    ProceduralControlParams controlParams;
    controlParams.seed = 1337;
    const ProceduralControls controls { controlParams };
    const MacroParams macro;
    MasterNetworkParams params;

    const auto rivers = masterRiversNear(
        controls, macro, params, -24000.0f, -24000.0f, 24000.0f,
        24000.0f);
    u32 toSea = 0;
    f64 trunkAligned = 0.0;
    u64 nodes = 0;
    f64 totalLength = 0.0;
    // Distinct SYSTEMS: courses sharing a mouth (within a texel) are
    // one fleuve with tributaries, not several fleuves.
    vector<std::pair<f32, f32>> mouths;
    f32 longest = 0.0f;
    for (const MasterRiver& river : rivers) {
        toSea += river.reachesSea;
        f32 length = 0.0f;
        for (size_t k = 1; k < river.nodes.size(); ++k) {
            length += std::hypot(
                river.nodes[k].x - river.nodes[k - 1].x,
                river.nodes[k].z - river.nodes[k - 1].z);
        }
        totalLength += length;
        longest = glm::max(longest, length);
        for (const MasterNode& node : river.nodes) {
            trunkAligned += controls.at(node.x, node.z).trunk > 0.3f;
            ++nodes;
        }
        bool merged = false;
        for (const auto& mouth : mouths) {
            if (std::hypot(mouth.first - river.nodes.back().x,
                           mouth.second - river.nodes.back().z) <
                600.0f) {
                merged = true;
                break;
            }
        }
        if (!merged) {
            mouths.push_back(
                { river.nodes.back().x, river.nodes.back().z });
        }
    }
    MESSAGE("fleuve systems in +/-24 km: ", mouths.size(), " (",
            rivers.size(), " courses, reaching sea ", toSea,
            "), longest course ", longest / 1000.0f,
            " km, total fleuve-tier length ", totalLength / 1000.0f,
            " km, nodes on trunk corridors ",
            nodes ? 100.0 * trunkAligned / nodes : 0.0, "%");
    CHECK(true);
}
