#include <doctest/doctest.h>

#include <array>
#include <map>

#include "engine/dungeon/MeshExtract.hpp"

using namespace dungeon;

namespace {

// An air bubble in rock: negative inside the sphere.
f32 sphereDensity(const Vec3& p) {
    const Vec3 c { 8.0f, 8.0f, 8.0f };
    return glm::length(p - c) - 5.5f;
}

using EdgeKey = std::array<f32, 6>;

EdgeKey edgeKey(const Vec3& a, const Vec3& b) {
    const bool swap = a.x != b.x ? a.x > b.x : (a.y != b.y ? a.y > b.y
                                                           : a.z > b.z);
    const Vec3& lo = swap ? b : a;
    const Vec3& hi = swap ? a : b;
    return { lo.x, lo.y, lo.z, hi.x, hi.y, hi.z };
}

// Edge -> number of triangles using it, keyed by exact vertex positions
// (the seam contract makes shared positions bit-identical).
std::map<EdgeKey, i32> edgeUse(const render::MeshData& mesh) {
    std::map<EdgeKey, i32> use;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3& a = mesh.vertices[mesh.indices[i]].position;
        const Vec3& b = mesh.vertices[mesh.indices[i + 1]].position;
        const Vec3& c = mesh.vertices[mesh.indices[i + 2]].position;
        ++use[edgeKey(a, b)];
        ++use[edgeKey(b, c)];
        ++use[edgeKey(c, a)];
    }
    return use;
}

} // namespace

TEST_CASE("dungeon mesh: a sphere extracts closed and correctly oriented") {
    const Vec3 lo { -2.0f, -2.0f, -2.0f };
    const Vec3 hi { 18.0f, 18.0f, 18.0f };
    const render::MeshData mesh = extractChunkMesh(sphereDensity, lo, hi, 1.0f);
    REQUIRE_FALSE(mesh.vertices.empty());
    REQUIRE(mesh.indices.size() % 3 == 0);

    // Watertight: every edge is shared by exactly two triangles.
    for (const auto& [key, count] : edgeUse(mesh)) {
        CHECK(count == 2);
    }

    // Air is inside the sphere: vertex normals point at the center, and each
    // triangle's geometric normal agrees with its vertices' normals.
    const Vec3 center { 8.0f, 8.0f, 8.0f };
    for (const render::MeshVertex& v : mesh.vertices) {
        CHECK(glm::dot(v.normal, center - v.position) > 0.0f);
    }
    i32 agreeing = 0;
    i32 total = 0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const render::MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const render::MeshVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const render::MeshVertex& c = mesh.vertices[mesh.indices[i + 2]];
        const Vec3 geometric = glm::cross(b.position - a.position,
                                          c.position - a.position);
        const Vec3 shading = a.normal + b.normal + c.normal;
        ++total;
        if (glm::dot(geometric, shading) > 0.0f) {
            ++agreeing;
        }
    }
    CHECK(agreeing == total);
}

TEST_CASE("dungeon mesh: adjacent chunks are watertight across the seam") {
    const f32 voxel = 1.0f;
    const render::MeshData left = extractChunkMesh(
        sphereDensity, { -2.0f, -2.0f, -2.0f }, { 8.0f, 18.0f, 18.0f }, voxel);
    const render::MeshData right = extractChunkMesh(
        sphereDensity, { 8.0f, -2.0f, -2.0f }, { 18.0f, 18.0f, 18.0f }, voxel);
    REQUIRE_FALSE(left.vertices.empty());
    REQUIRE_FALSE(right.vertices.empty());

    // Merged, the two chunks must close the sphere: every edge is used
    // exactly twice across the union (bit-identical seam vertices).
    std::map<EdgeKey, i32> use = edgeUse(left);
    for (const auto& [key, count] : edgeUse(right)) {
        use[key] += count;
    }
    for (const auto& [key, count] : use) {
        CHECK(count == 2);
    }
}

TEST_CASE("dungeon mesh: extraction is deterministic") {
    const Vec3 lo { -2.0f, -2.0f, -2.0f };
    const Vec3 hi { 18.0f, 18.0f, 18.0f };
    const render::MeshData a = extractChunkMesh(sphereDensity, lo, hi, 0.9f);
    const render::MeshData b = extractChunkMesh(sphereDensity, lo, hi, 0.9f);
    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.indices == b.indices);
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        CHECK(a.vertices[i].position == b.vertices[i].position);
        CHECK(a.vertices[i].normal == b.vertices[i].normal);
    }
}

TEST_CASE("dungeon mesh: an all-rock chunk yields an empty mesh") {
    const render::MeshData mesh = extractChunkMesh(
        [](const Vec3&) { return 1.0f; }, { 0.0f, 0.0f, 0.0f },
        { 8.0f, 8.0f, 8.0f }, 1.0f);
    CHECK(mesh.vertices.empty());
    CHECK(mesh.indices.empty());
}
