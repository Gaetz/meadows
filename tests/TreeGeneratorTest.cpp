#include <doctest/doctest.h>

#include <cstring>

#include "engine/render/landscape/TreeGenerator.hpp"

using render::MeshData;
using render::TreeMeshes;

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

TEST_CASE("same seed generates a bit-identical tree (body and leaves)") {
    const TreeMeshes a = render::generateTree(42);
    const TreeMeshes b = render::generateTree(42);
    CHECK(sameMesh(a.body, b.body));
    CHECK(sameMesh(a.leaves, b.leaves));
}

TEST_CASE("different seeds generate different trees") {
    const TreeMeshes a = render::generateTree(1);
    const TreeMeshes b = render::generateTree(2);
    CHECK_FALSE(sameMesh(a.body, b.body));
}

TEST_CASE("generated trees are well-formed meshes") {
    for (u32 seed : { 7u, 977u, 123456u }) {
        const TreeMeshes tree = render::generateTree(seed);
        checkWellFormed(tree.body);
        checkWellFormed(tree.leaves);
        for (const render::MeshVertex& vertex : tree.body.vertices) {
            CHECK(glm::length(vertex.normal) ==
                  doctest::Approx(1.0f).epsilon(0.01));
            CHECK(vertex.uv.x >= 0.0f);
            CHECK(vertex.uv.x <= 1.0f);
        }
        // Leaf cards: static quads whose four corners share ONE unit
        // spherical normal (blob-center direction). Atlas uvs in [0,1].
        for (const render::MeshVertex& vertex : tree.leaves.vertices) {
            CHECK(glm::length(vertex.normal) ==
                  doctest::Approx(1.0f).epsilon(0.01));
            CHECK(vertex.uv.x >= 0.0f);
            CHECK(vertex.uv.x <= 1.0f);
            CHECK(vertex.uv.y >= 0.0f);
            CHECK(vertex.uv.y <= 1.0f);
        }
        // Cards are quads: 6 indices / 4 vertices each.
        CHECK(tree.leaves.vertices.size() % 4 == 0);
        CHECK(tree.leaves.indices.size() ==
              tree.leaves.vertices.size() / 4 * 6);
        for (size_t card = 0; card + 3 < tree.leaves.vertices.size();
             card += 4) {
            for (u32 corner = 1; corner < 4; ++corner) {
                CHECK(tree.leaves.vertices[card].normal ==
                      tree.leaves.vertices[card + corner].normal);
            }
        }
    }
}

TEST_CASE("leaf texture atlas is deterministic and has coverage") {
    const vector<u8> a = render::buildLeafTexturePixels();
    const vector<u8> b = render::buildLeafTexturePixels();
    REQUIRE(a.size() == static_cast<size_t>(render::kLeafTextureSize) *
                            render::kLeafTextureSize * 4);
    CHECK(a == b);
    // The bouquets must actually cover a good part of each atlas cell.
    u64 opaque = 0;
    for (size_t i = 3; i < a.size(); i += 4) {
        if (a[i] > 128) {
            ++opaque;
        }
    }
    const u64 total = a.size() / 4;
    CHECK(opaque > total / 8);
    CHECK(opaque < total);
}
