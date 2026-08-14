#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "engine/assets/CookedMesh.hpp"

namespace {

render::MeshData sampleMesh() {
    render::MeshData mesh;
    mesh.vertices = {
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f },
          { 1.0f, 0.5f, 0.25f } },
        { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f },
          { 0.5f, 0.5f, 0.5f } },
        { { 0.0f, 2.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f },
          { 0.0f, 0.0f, 1.0f } },
    };
    mesh.indices = { 0, 1, 2 };
    return mesh;
}

std::filesystem::path tempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST_CASE("cooked mesh: round-trips vertices and indices verbatim") {
    const auto path = tempPath("meadows_cmesh_roundtrip.cmesh");
    const render::MeshData mesh = sampleMesh();
    REQUIRE(assets::saveCookedMesh(path, mesh, 3));
    const auto loaded = assets::loadCookedMesh(path, 3);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->vertices.size() == mesh.vertices.size());
    REQUIRE(loaded->indices.size() == mesh.indices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        CHECK(loaded->vertices[i].position == mesh.vertices[i].position);
        CHECK(loaded->vertices[i].normal == mesh.vertices[i].normal);
        CHECK(loaded->vertices[i].uv == mesh.vertices[i].uv);
        CHECK(loaded->vertices[i].color == mesh.vertices[i].color);
    }
    CHECK(loaded->indices == mesh.indices);
    std::filesystem::remove(path);
}

TEST_CASE("cooked mesh: refuses a stale content version") {
    const auto path = tempPath("meadows_cmesh_version.cmesh");
    REQUIRE(assets::saveCookedMesh(path, sampleMesh(), 1));
    CHECK_FALSE(assets::loadCookedMesh(path, 2).has_value());
    CHECK(assets::loadCookedMesh(path, 1).has_value());
    std::filesystem::remove(path);
}

TEST_CASE("cooked mesh: refuses foreign or truncated files") {
    const auto path = tempPath("meadows_cmesh_garbage.cmesh");
    {
        std::ofstream file { path, std::ios::binary | std::ios::trunc };
        file << "GLTF this is not a cooked mesh";
    }
    CHECK_FALSE(assets::loadCookedMesh(path, 0).has_value());
    std::filesystem::remove(path);
    CHECK_FALSE(assets::loadCookedMesh(path, 0).has_value()); // missing file
}

TEST_CASE("cooked mesh: refuses malformed meshes at save") {
    const auto path = tempPath("meadows_cmesh_malformed.cmesh");
    render::MeshData empty;
    CHECK_FALSE(assets::saveCookedMesh(path, empty, 0));
    render::MeshData badIndices = sampleMesh();
    badIndices.indices = { 0, 1 }; // not a triangle list
    CHECK_FALSE(assets::saveCookedMesh(path, badIndices, 0));
}
