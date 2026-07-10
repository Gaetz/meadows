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

// The disk cache (dev ask 2026-07-10: authored meshes take seconds to
// bake — cache across sessions, validate by mtime/size, prune orphans).
#include <filesystem>
#include <fstream>

#include "engine/assets/VertexAoCache.hpp"

namespace {

namespace fs = std::filesystem;

struct TempCacheDir {
    fs::path dir;
    fs::path source;
    TempCacheDir() {
        dir = fs::temp_directory_path() / "meadows-ao-cache-test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        // A real on-disk "source mesh" file (the cache fingerprints it).
        source = dir / "source-mesh.gltf";
        std::ofstream(source, std::ios::binary) << "fake gltf bytes";
    }
    ~TempCacheDir() { fs::remove_all(dir); }
};

} // namespace

TEST_CASE("vertex AO cache: save/load round-trip, stale on param change") {
    TempCacheDir temp;
    const assets::VertexAoBakeDesc desc { 16, 2.5f };
    const vector<f32> occlusion { 0.0f, 0.25f, 0.5f, 1.0f };

    // Nothing cached yet.
    CHECK_FALSE(assets::loadVertexAoCache(temp.dir, temp.source, desc, 4)
                    .has_value());
    REQUIRE(assets::saveVertexAoCache(temp.dir, temp.source, desc,
                                      occlusion));
    const auto loaded =
        assets::loadVertexAoCache(temp.dir, temp.source, desc, 4);
    REQUIRE(loaded.has_value());
    CHECK((*loaded)[1] == doctest::Approx(0.25f));
    CHECK((*loaded)[3] == doctest::Approx(1.0f));

    // Different params, vertex count or a rewritten source = stale.
    CHECK_FALSE(assets::loadVertexAoCache(temp.dir, temp.source,
                                          { 8, 2.5f }, 4)
                    .has_value());
    CHECK_FALSE(assets::loadVertexAoCache(temp.dir, temp.source, desc, 5)
                    .has_value());
    std::ofstream(temp.source, std::ios::binary)
        << "different bytes entirely";
    CHECK_FALSE(assets::loadVertexAoCache(temp.dir, temp.source, desc, 4)
                    .has_value());
}

TEST_CASE("vertex AO cache: applyCached bakes once then reuses the entry") {
    TempCacheDir temp;
    const auto build = [] {
        render::MeshData mesh;
        appendQuad(mesh, { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
        appendQuad(mesh, { -0.1f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f });
        return mesh;
    };
    render::MeshData first = build();
    assets::applyCachedVertexAo(first, temp.source, temp.dir, 0.8f,
                                { 16, 1.5f });
    // The entry now exists and drives the second apply: TAMPER it via
    // the public API (same key, different values) — if the second apply
    // LOADS (instead of re-baking), the tampered values show up.
    vector<f32> tampered(first.vertices.size(), 0.5f);
    REQUIRE(assets::saveVertexAoCache(temp.dir, temp.source,
                                      { 16, 1.5f }, tampered));
    render::MeshData second = build();
    assets::applyCachedVertexAo(second, temp.source, temp.dir, 0.8f,
                                { 16, 1.5f });
    for (const render::MeshVertex& vertex : second.vertices) {
        CHECK(vertex.color.r == doctest::Approx(1.0f - 0.8f * 0.5f));
    }
}

TEST_CASE("vertex AO cache: prune drops orphans, keeps live entries") {
    TempCacheDir temp;
    const assets::VertexAoBakeDesc desc {};
    const vector<f32> occlusion { 0.1f };
    // A live entry (source exists) and an orphan (source deleted after).
    REQUIRE(assets::saveVertexAoCache(temp.dir, temp.source, desc,
                                      occlusion));
    const fs::path ghost = temp.dir / "ghost-mesh.gltf";
    std::ofstream(ghost, std::ios::binary) << "soon gone";
    REQUIRE(assets::saveVertexAoCache(temp.dir, ghost, desc, occlusion));
    fs::remove(ghost);

    CHECK(assets::pruneVertexAoCache(temp.dir) == 1);
    // The live entry survived and still loads.
    CHECK(assets::loadVertexAoCache(temp.dir, temp.source, desc, 1)
              .has_value());
    // Idempotent.
    CHECK(assets::pruneVertexAoCache(temp.dir) == 0);
}

TEST_CASE("vertex AO cache: content-keyed entries bake once, survive prune") {
    TempCacheDir temp;
    const auto build = [] {
        render::MeshData mesh;
        appendQuad(mesh, { 0.0f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
        appendQuad(mesh, { -0.1f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f });
        return mesh;
    };
    // First apply bakes + stores one entry (no source file involved).
    render::MeshData first = build();
    assets::applyContentKeyedVertexAo(first, temp.dir, 0.8f, { 16, 1.5f });
    u32 aoEntries = 0;
    for (const auto& entry : fs::directory_iterator(temp.dir)) {
        aoEntries += entry.path().extension() == ".ao" ? 1u : 0u;
    }
    CHECK(aoEntries == 1);

    // Keyed entries are exempt from the prune (no source to check).
    CHECK(assets::pruneVertexAoCache(temp.dir) == 0);

    // The second apply LOADS: identical result, entry untouched.
    render::MeshData second = build();
    assets::applyContentKeyedVertexAo(second, temp.dir, 0.8f, { 16, 1.5f });
    for (size_t i = 0; i < first.vertices.size(); ++i) {
        CHECK(second.vertices[i].color.r ==
              doctest::Approx(first.vertices[i].color.r));
    }

    // DIFFERENT geometry = different key = a second entry.
    render::MeshData other;
    appendQuad(other, { 5.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
               { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f });
    appendQuad(other, { 4.9f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
               { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f });
    assets::applyContentKeyedVertexAo(other, temp.dir, 0.8f, { 16, 1.5f });
    aoEntries = 0;
    for (const auto& entry : fs::directory_iterator(temp.dir)) {
        aoEntries += entry.path().extension() == ".ao" ? 1u : 0u;
    }
    CHECK(aoEntries == 2);
}
