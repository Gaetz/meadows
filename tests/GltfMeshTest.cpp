#include <doctest/doctest.h>

#include <cstring>

#include "engine/assets/GltfMesh.hpp"

using render::MeshData;

namespace {

// Minimal valid glTF: one triangle (POSITION only, no indices, no normals)
// in an embedded data-URI buffer, under a translated node, with a colored
// material. Exercises: parse, buffer decode, node transform, sequential
// index synthesis, normal reconstruction, baseColorFactor -> vertex color.
constexpr const char* kTriangleGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [{ "nodes": [0] }],
  "nodes": [{ "mesh": 0, "translation": [2.0, 3.0, 4.0] }],
  "meshes": [{ "primitives": [
    { "attributes": { "POSITION": 0 }, "material": 0 } ] }],
  "materials": [{ "pbrMetallicRoughness":
    { "baseColorFactor": [0.5, 0.25, 0.125, 1.0] } }],
  "accessors": [{
    "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
    "min": [0.0, 0.0, 0.0], "max": [1.0, 0.0, 1.0] }],
  "bufferViews": [{ "buffer": 0, "byteOffset": 0, "byteLength": 36 }],
  "buffers": [{ "byteLength": 36, "uri":
    "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/" }]
})";

} // namespace

TEST_CASE("in-memory glTF produces a well-formed MeshData") {
    const auto mesh = assets::loadGltfMeshFromMemory(
        kTriangleGltf, std::strlen(kTriangleGltf));
    REQUIRE(mesh.has_value());
    REQUIRE(mesh->vertices.size() == 3);
    REQUIRE(mesh->indices.size() == 3);
    for (const u32 index : mesh->indices) {
        CHECK(index < mesh->vertices.size());
    }

    // Node translation [2,3,4] applied to the authored (0,0,0).
    CHECK(mesh->vertices[0].position.x == doctest::Approx(2.0f));
    CHECK(mesh->vertices[0].position.y == doctest::Approx(3.0f));
    CHECK(mesh->vertices[0].position.z == doctest::Approx(4.0f));

    // baseColorFactor rides on the vertex color.
    CHECK(mesh->vertices[0].color.r == doctest::Approx(0.5f));
    CHECK(mesh->vertices[0].color.g == doctest::Approx(0.25f));
    CHECK(mesh->vertices[0].color.b == doctest::Approx(0.125f));

    // Normals were reconstructed (no NORMAL attribute): unit length,
    // identical across the flat triangle.
    for (const render::MeshVertex& vertex : mesh->vertices) {
        CHECK(glm::length(vertex.normal) ==
              doctest::Approx(1.0f).epsilon(0.01));
    }
    CHECK(mesh->vertices[0].normal.y ==
          doctest::Approx(mesh->vertices[1].normal.y));
}

TEST_CASE("malformed glTF is rejected, not crashed on") {
    const char* garbage = "{ \"asset\": { \"version\": \"2.0\" } }";
    CHECK_FALSE(
        assets::loadGltfMeshFromMemory(garbage, std::strlen(garbage))
            .has_value());
    const char* notJson = "definitely not gltf";
    CHECK_FALSE(
        assets::loadGltfMeshFromMemory(notJson, std::strlen(notJson))
            .has_value());
}

TEST_CASE("normalizeMesh recenters the footprint and grounds the base") {
    auto mesh = assets::loadGltfMeshFromMemory(kTriangleGltf,
                                               std::strlen(kTriangleGltf));
    REQUIRE(mesh.has_value());
    assets::normalizeMesh(*mesh, 2.0f);

    Vec3 lo = mesh->vertices[0].position;
    Vec3 hi = lo;
    for (const render::MeshVertex& vertex : mesh->vertices) {
        lo = glm::min(lo, vertex.position);
        hi = glm::max(hi, vertex.position);
    }
    // Largest dimension scaled to the 2 m target...
    CHECK(std::max({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z }) ==
          doctest::Approx(2.0f));
    // ...base sits on y = 0, XZ footprint centered on the origin.
    CHECK(lo.y == doctest::Approx(0.0f));
    CHECK(lo.x == doctest::Approx(-hi.x));
    CHECK(lo.z == doctest::Approx(-hi.z));
}
