#include <doctest/doctest.h>

#include <cstring>

#include "engine/render/landscape/TreeGenerator.hpp"

using render::MeshData;

TEST_CASE("same seed generates a bit-identical tree") {
    const MeshData a = render::generateTree(42);
    const MeshData b = render::generateTree(42);
    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.indices.size() == b.indices.size());
    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(render::MeshVertex)) == 0);
    CHECK(std::memcmp(a.indices.data(), b.indices.data(),
                      a.indices.size() * sizeof(u32)) == 0);
}

TEST_CASE("different seeds generate different trees") {
    const MeshData a = render::generateTree(1);
    const MeshData b = render::generateTree(2);
    const bool sameLayout = a.vertices.size() == b.vertices.size();
    const bool identical =
        sameLayout &&
        std::memcmp(a.vertices.data(), b.vertices.data(),
                    a.vertices.size() * sizeof(render::MeshVertex)) == 0;
    CHECK_FALSE(identical);
}

TEST_CASE("generated trees are well-formed meshes") {
    for (u32 seed : { 7u, 977u, 123456u }) {
        const MeshData mesh = render::generateTree(seed);
        REQUIRE(!mesh.vertices.empty());
        REQUIRE(!mesh.indices.empty());
        CHECK(mesh.indices.size() % 3 == 0);
        for (const u32 index : mesh.indices) {
            CHECK(index < mesh.vertices.size());
        }
        for (const render::MeshVertex& vertex : mesh.vertices) {
            CHECK(glm::length(vertex.normal) ==
                  doctest::Approx(1.0f).epsilon(0.01));
            CHECK(vertex.uv.x >= 0.0f);
            CHECK(vertex.uv.x <= 1.0f);
        }
    }
}
