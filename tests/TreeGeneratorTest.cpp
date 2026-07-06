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

TEST_CASE("generated trees are well-formed solid-canopy meshes (brick 27)") {
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
