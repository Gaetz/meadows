#include <doctest/doctest.h>

#include "engine/assets/VertexAo.hpp"

// Option B (2026-07-10): per-asset baked vertex AO — the grounding that
// replaced screen-space AO in the default look. Pure geometry in, vertex
// colors out; deterministic (fixed golden-spiral rays).

using render::MeshData;
using render::MeshVertex;

namespace {

void appendQuad(MeshData& mesh, const Vec3& origin, const Vec3& edgeU,
                const Vec3& edgeV, const Vec3& normal) {
    const u32 base = static_cast<u32>(mesh.vertices.size());
    const Vec3 white { 1.0f, 1.0f, 1.0f };
    mesh.vertices.push_back({ origin, normal, { 0.0f, 0.0f }, white });
    mesh.vertices.push_back(
        { origin + edgeU, normal, { 1.0f, 0.0f }, white });
    mesh.vertices.push_back(
        { origin + edgeU + edgeV, normal, { 1.0f, 1.0f }, white });
    mesh.vertices.push_back(
        { origin + edgeV, normal, { 0.0f, 1.0f }, white });
    for (const u32 i : { 0u, 1u, 2u, 0u, 2u, 3u }) {
        mesh.indices.push_back(base + i);
    }
}

} // namespace

TEST_CASE("vertex AO: a flat quad has nothing to occlude itself") {
    MeshData mesh;
    appendQuad(mesh, { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f },
               { 0.0f, 0.0f, 2.0f }, { 0.0f, 1.0f, 0.0f });
    assets::bakeVertexAo(mesh, 0.8f);
    for (const MeshVertex& vertex : mesh.vertices) {
        CHECK(vertex.color.r == doctest::Approx(1.0f));
    }
}

TEST_CASE("vertex AO: geometry close overhead darkens, distant does not") {
    // Floor (normal up) + a wall RISING JUST IN FRONT of its near edge
    // (x = -0.1): the near floor vertices stare at it, the far edge
    // (x = 2) has it beyond maxDistance. (A vertex EXACTLY in its
    // occluder's plane cannot intersect it — a known bake limitation at
    // perfect folds, which is why the wall stands slightly offset here,
    // like any real crevice/canopy interior.)
    const auto build = [] {
        MeshData mesh;
        appendQuad(mesh, { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
        appendQuad(mesh, { -0.1f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f });
        return mesh;
    };
    MeshData mesh = build();
    assets::bakeVertexAo(mesh, 0.8f, 16, 1.5f);

    const f32 nearWall = mesh.vertices[0].color.r; // floor @ x=0
    const f32 farEdge = mesh.vertices[1].color.r;  // floor @ x=2
    CHECK(nearWall < farEdge - 0.05f);
    CHECK(farEdge == doctest::Approx(1.0f)); // wall beyond maxDistance

    // Determinism: same input bakes to the same values.
    MeshData again = build();
    assets::bakeVertexAo(again, 0.8f, 16, 1.5f);
    CHECK(again.vertices[0].color.r == doctest::Approx(nearWall));
}

TEST_CASE("vertex AO: zero strength is a strict no-op") {
    MeshData mesh;
    appendQuad(mesh, { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
               { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
    appendQuad(mesh, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
               { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f });
    assets::bakeVertexAo(mesh, 0.0f);
    for (const MeshVertex& vertex : mesh.vertices) {
        CHECK(vertex.color.r == doctest::Approx(1.0f));
    }
}
