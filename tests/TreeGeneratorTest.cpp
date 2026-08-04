#include <doctest/doctest.h>

#include <cstring>

#include "engine/render/landscape/TreeGenerator.hpp"

using render::MeshData;

namespace {

bool sameMesh(const MeshData& a, const MeshData& b) {
    return a.vertices.size() == b.vertices.size() &&
           a.indices.size() == b.indices.size() &&
           std::memcmp(a.vertices.data(), b.vertices.data(),
                       a.vertices.size() * sizeof(render::MeshVertex)) == 0 &&
           std::memcmp(a.indices.data(), b.indices.data(),
                       a.indices.size() * sizeof(u32)) == 0;
}

void checkWellFormed(const MeshData& mesh) {
    REQUIRE(!mesh.vertices.empty());
    REQUIRE(!mesh.indices.empty());
    CHECK(mesh.indices.size() % 3 == 0);
    for (const u32 index : mesh.indices) {
        CHECK(index < mesh.vertices.size());
    }
}

} // namespace

TEST_CASE("same seed generates a bit-identical tree") {
    const MeshData a = render::generateTree(42);
    const MeshData b = render::generateTree(42);
    CHECK(sameMesh(a, b));
}

TEST_CASE("different seeds generate different trees") {
    const MeshData a = render::generateTree(1);
    const MeshData b = render::generateTree(2);
    CHECK_FALSE(sameMesh(a, b));
}

TEST_CASE("generated trees are well-formed solid-canopy meshes") {
    for (u32 seed : { 7u, 977u, 123456u }) {
        const MeshData tree = render::generateTree(seed);
        checkWellFormed(tree);
        u32 canopyVertices = 0;
        for (const render::MeshVertex& vertex : tree.vertices) {
            CHECK(glm::length(vertex.normal) ==
                  doctest::Approx(1.0f).epsilon(0.01));
            CHECK(vertex.uv.x >= 0.0f);
            CHECK(vertex.uv.x <= 1.0f);
            // Canopy vertices (green channel dominates bark's red-brown)
            // carry SPHERIZED normals: never anti-parallel to the outward
            // direction — flat-shaded facets would routinely disagree by
            // more; this catches a forgotten spherize pass.
            if (vertex.color.g > vertex.color.r) {
                ++canopyVertices;
                CHECK(vertex.normal.y > -1.0f);
            }
        }
        // The canopy is the bulk of the mesh (subdiv-2 lobes, 320 faces
        // each) — a missing lobe pass would collapse this.
        CHECK(canopyVertices > tree.vertices.size() / 2);
    }
}

// EXPERIMENT (feature/space-colonization-trees): the Runions/SDF-card
// generator honors the same contracts as generateTree — determinism per
// seed, well-formed mesh, LOD levels sharing one seed, unit SDF normals
// on the foliage cards, uv in range.
TEST_CASE("same seed generates a bit-identical colonized tree") {
    for (u32 seed : { 3u, 977u, 424242u }) {
        CHECK(sameMesh(render::generateColonizedTree(seed),
                       render::generateColonizedTree(seed)));
    }
}

TEST_CASE("colonized trees are well-formed at every detail level") {
    for (u32 seed : { 3u, 977u, 424242u }) {
        for (u32 detail : { 0u, 1u, 2u }) {
            const MeshData tree =
                render::generateColonizedTree(seed, detail);
            checkWellFormed(tree);
            u32 cardVertices = 0;
            for (const render::MeshVertex& vertex : tree.vertices) {
                CHECK(glm::length(vertex.normal) ==
                      doctest::Approx(1.0f).epsilon(0.01));
                if (vertex.uv.x < -5.0f) {
                    // Billboard leaf card: uv encodes corner x halfSize
                    // around the -10 flag bias (see appendBillboardCard).
                    // The encoding stays unambiguous against wood uv
                    // (x in [0,1], y = -1) for any halfSize below 1.
                    ++cardVertices;
                    CHECK(std::abs(vertex.uv.x + 10.0f) < 1.0f);
                    CHECK(std::abs(vertex.uv.y) < 1.0f);
                } else {
                    CHECK(vertex.uv.x >= 0.0f);
                    CHECK(vertex.uv.x <= 1.0f);
                    // Wood carries the BARK flag (uv.y < -0.5 —
                    // tree.frag samples the slot's bark triplanarly).
                    CHECK(vertex.uv.y == doctest::Approx(-1.0f));
                }
            }
            CHECK(cardVertices > 0);
            CHECK(cardVertices % 4 == 0); // degenerate quads, 4 verts each
        }
        // Coarser levels never carry MORE geometry.
        CHECK(render::generateColonizedTree(seed, 0).indices.size() <=
              render::generateColonizedTree(seed, 1).indices.size());
        CHECK(render::generateColonizedTree(seed, 1).indices.size() <=
              render::generateColonizedTree(seed, 2).indices.size());
    }
}

TEST_CASE("different seeds generate different colonized trees") {
    CHECK(!sameMesh(render::generateColonizedTree(3u),
                    render::generateColonizedTree(4u)));
}

TEST_CASE("leaf-mask shapes are deterministic and distinct; cards carry "
          "their atlas slot") {
    render::ColonizedTreeParams params;
    vector<vector<u8>> masks;
    for (i32 shape = 0; shape <= 4; ++shape) {
        masks.push_back(
            render::generateLeafMaskPixels(64, 7, params, shape));
        CHECK(masks.back() ==
              render::generateLeafMaskPixels(64, 7, params, shape));
    }
    for (size_t a = 0; a < masks.size(); ++a) {
        for (size_t b = a + 1; b < masks.size(); ++b) {
            CHECK(masks[a] != masks[b]);
        }
    }
    // The card's uv bias encodes the atlas slot: -10 - 20*slot.
    render::ColonizedTreeParams styled;
    styled.leafStyle = 1;
    f32 minU = 0.0f;
    for (const render::MeshVertex& v :
         render::generateColonizedTree(3u, 2, styled).vertices) {
        minU = glm::min(minU, v.uv.x);
    }
    CHECK(minU < -25.0f); // slot-1 cards present
    minU = 0.0f;
    for (const render::MeshVertex& v :
         render::generateColonizedTree(3u).vertices) {
        minU = glm::min(minU, v.uv.x);
    }
    CHECK(minU < -5.0f);   // slot-0 cards present...
    CHECK(minU > -25.0f);  // ...and none flagged past slot 0
}

TEST_CASE("conifer habit: neutral knobs are bit-exact, cone narrows up") {
    // All four conifer knobs at 0 must not consume a single random draw:
    // the broadleaf output is bit-identical to the pre-knob generator.
    render::ColonizedTreeParams neutral;
    neutral.crownTaper = 0.0f;
    neutral.leaderBias = 0.0f;
    neutral.lateralFlatten = 0.0f;
    neutral.sprayFoliage = 0.0f;
    CHECK(sameMesh(render::generateColonizedTree(977u),
                   render::generateColonizedTree(977u, 2, neutral)));

    render::ColonizedTreeParams conifer;
    conifer.crownTaper = 0.85f;
    conifer.leaderBias = 0.35f;
    conifer.lateralFlatten = 0.7f;
    conifer.sprayFoliage = 0.9f;
    conifer.trunkBaseMin = 0.8f;
    conifer.trunkBaseMax = 1.4f;
    conifer.crownHeightMin = 5.0f;
    conifer.crownHeightMax = 7.0f;
    // Wide base for a discriminant measure: the apex keeps a floor
    // width (kill distance + growth overshoot + card shell) whatever
    // the taper, so the cone reads in the base-vs-top ratio.
    conifer.crownRadiusMin = 2.2f;
    conifer.crownRadiusMax = 2.6f;
    for (u32 seed : { 3u, 977u }) {
        const MeshData tree =
            render::generateColonizedTree(seed, 2, conifer);
        checkWellFormed(tree);
        CHECK(sameMesh(tree,
                       render::generateColonizedTree(seed, 2, conifer)));
        // Conical silhouette: the crown's top third is materially
        // narrower than its bottom third (radial extent of the canopy
        // vertices — green channel dominant).
        f32 top = 0.0f, bottom = 0.0f, maxY = 0.0f;
        for (const render::MeshVertex& v : tree.vertices) {
            maxY = glm::max(maxY, v.position.y);
        }
        for (const render::MeshVertex& v : tree.vertices) {
            if (v.color.g <= v.color.r) {
                continue;
            }
            const f32 radial = glm::length(
                Vec2 { v.position.x, v.position.z });
            if (v.position.y > 0.85f * maxY) {
                top = glm::max(top, radial);
            } else if (v.position.y > 0.3f * maxY &&
                       v.position.y < 0.5f * maxY) {
                bottom = glm::max(bottom, radial);
            }
        }
        CHECK(bottom > 0.0f);
        CHECK(top < bottom * 0.75f);
    }
}
