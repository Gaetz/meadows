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
                    // Billboard leaf card: uv encodes the corner around
                    // the -10 flag bias (see appendBillboardCard).
                    ++cardVertices;
                    CHECK(std::abs(vertex.uv.x + 10.0f) < 0.1f);
                    CHECK(std::abs(vertex.uv.y) < 0.1f);
                } else {
                    CHECK(vertex.uv.x >= 0.0f);
                    CHECK(vertex.uv.x <= 1.0f);
                    CHECK(vertex.uv.y >= 0.0f);
                    CHECK(vertex.uv.y <= 1.0f);
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
