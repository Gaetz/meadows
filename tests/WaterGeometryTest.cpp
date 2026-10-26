#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterGeometry.hpp"
#include "engine/terrain/WaterSim.hpp"

// The baked water meshers are pure CPU functions of the published bodies
// + terrain params, built on a worker and uploaded verbatim: a refactor
// is provably output-preserving only if the raw vertex and index bytes
// do not move. Same harness as the scatter goldens (GrassScatterTest).

namespace {

using render::LakeSurface;
using render::RiverNode;
using render::RiverSurface;
using render::TerrainParams;
using render::WaterBodies;
using render::terrain::FarWaterSet;

u64 fnv1a(u64 h, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

u64 hashMesh(const vector<f32>& verts, const vector<u32>& indices) {
    u64 h = 1469598103934665603ull; // FNV-1a offset basis
    const u64 vcount = verts.size();
    const u64 icount = indices.size();
    h = fnv1a(h, &vcount, sizeof(vcount));
    h = fnv1a(h, verts.data(), verts.size() * sizeof(f32));
    h = fnv1a(h, &icount, sizeof(icount));
    h = fnv1a(h, indices.data(), indices.size() * sizeof(u32));
    return h;
}

// A masked lake whose level sits a little above the local ground (so
// the ring probes meet banks, shelves and crests), a maskless pond (the
// bbox-quad path) and a bending brook (the curvature subdivision path).
WaterBodies makeBodies(const TerrainParams& params) {
    WaterBodies bodies;
    LakeSurface lake;
    lake.minX = 96.0f;
    lake.minZ = 160.0f;
    lake.maskTexel = 8.0f;
    lake.maskWidth = 14;
    lake.maskHeight = 11;
    lake.maxX = lake.minX + static_cast<f32>(lake.maskWidth - 1) *
                                lake.maskTexel;
    lake.maxZ = lake.minZ + static_cast<f32>(lake.maskHeight - 1) *
                                lake.maskTexel;
    lake.mask.assign(static_cast<size_t>(lake.maskWidth) * lake.maskHeight,
                     0);
    for (u32 r = 0; r < lake.maskHeight; ++r) {
        for (u32 c = 0; c < lake.maskWidth; ++c) {
            // Elliptic blob with a notch: a convex shore, a pinch and
            // an inlet all appear in the ring.
            const f32 u = (static_cast<f32>(c) - 6.5f) / 6.0f;
            const f32 v = (static_cast<f32>(r) - 5.0f) / 4.5f;
            const bool inside = u * u + v * v <= 1.0f;
            const bool notch = c >= 9 && c <= 10 && r >= 4 && r <= 6;
            lake.mask[static_cast<size_t>(r) * lake.maskWidth + c] =
                (inside && !notch) ? 1 : 0;
        }
    }
    const f32 cx = 0.5f * (lake.minX + lake.maxX);
    const f32 cz = 0.5f * (lake.minZ + lake.maxZ);
    lake.level = render::terrain::height(params, cx, cz) + 1.5f;
    lake.materialIndex = 2;
    bodies.lakes.push_back(lake);

    LakeSurface pond;
    pond.minX = 400.0f;
    pond.minZ = 400.0f;
    pond.maxX = 430.0f;
    pond.maxZ = 420.0f;
    pond.level = render::terrain::height(params, 415.0f, 410.0f) + 0.5f;
    bodies.lakes.push_back(pond);

    RiverSurface river;
    river.flowSpeed = 1.3f;
    river.materialIndex = 1;
    for (i32 i = 0; i < 9; ++i) {
        RiverNode node;
        const f32 t = static_cast<f32>(i);
        node.x = 200.0f + t * 12.0f;
        node.z = 300.0f + 18.0f * std::sin(t * 0.55f);
        node.surface = 40.0f - t * 0.6f;
        node.halfWidth = 1.5f + 0.1f * t;
        node.area = 1500.0f + 400.0f * t;
        river.nodes.push_back(node);
    }
    river.headIsSource = 1;
    bodies.rivers.push_back(river);
    return bodies;
}

FarWaterSet makeFarSet() {
    FarWaterSet set;
    FarWaterSet::Lake lake;
    lake.level = 12.0f;
    lake.minX = -3200.0f;
    lake.minZ = 2048.0f;
    lake.cell = 64.0f;
    lake.w = 7;
    lake.h = 4;
    lake.mask = { 0, 1, 1, 0, 1, 1, 1, //
                  1, 1, 1, 1, 1, 1, 0, //
                  0, 1, 1, 1, 1, 0, 0, //
                  0, 0, 1, 0, 0, 0, 0 };
    set.lakes.push_back(lake);
    FarWaterSet::Ribbon ribbon;
    ribbon.nodes = { { 0.0f, 0.0f, 30.0f, 6.0f },
                     { 64.0f, 10.0f, 29.0f, 6.5f },
                     { 64.0f, 10.0f, 29.0f, 6.5f }, // duplicate: skipped
                     { 128.0f, 30.0f, 28.0f, 7.0f },
                     { 190.0f, 70.0f, 27.0f, 7.5f } };
    set.ribbons.push_back(ribbon);
    return set;
}

} // namespace

TEST_CASE("water geometry: deterministic and populated") {
    const TerrainParams params; // seed 1337, pure procedural
    const WaterBodies bodies = makeBodies(params);
    vector<f32> verts;
    vector<u32> indices;
    render::terrain::buildLocalWaterGeometry(bodies, params, verts, indices);
    CHECK(!verts.empty());
    CHECK(indices.size() % 3 == 0);
    CHECK(verts.size() % render::terrain::kWaterVertexFloats == 0);
    const u32 vertexCount =
        static_cast<u32>(verts.size() / render::terrain::kWaterVertexFloats);
    for (const u32 i : indices) {
        CHECK(i < vertexCount);
    }
    vector<f32> again;
    vector<u32> againIdx;
    render::terrain::buildLocalWaterGeometry(bodies, params, again, againIdx);
    CHECK(hashMesh(verts, indices) == hashMesh(again, againIdx));

    // Per-pass functions append exactly what the whole build appends.
    vector<f32> perPass;
    vector<u32> perPassIdx;
    for (const LakeSurface& lake : bodies.lakes) {
        render::terrain::meshLakeSheet(lake, bodies, params, perPass,
                                       perPassIdx);
    }
    for (const RiverSurface& river : bodies.rivers) {
        render::terrain::meshRiverRibbon(river, perPass, perPassIdx);
    }
    CHECK(hashMesh(perPass, perPassIdx) == hashMesh(verts, indices));
}

TEST_CASE("water geometry: far sheets quad each row run and strip each node") {
    const FarWaterSet set = makeFarSet();
    vector<f32> verts;
    vector<u32> indices;
    render::terrain::buildFarWaterGeometry(set, verts, indices);
    // The mask holds 5 row RUNS of covered cells (2 + 1 + 1 + 1) -> 5
    // quads; every node whose tangent (ahead - behind) is non-zero emits
    // a vertex pair, the duplicate node included -> 5 pairs, 4 strip
    // segments of 2 triangles.
    const u32 lakeTriangles = 5 * 2;
    const u32 ribbonTriangles = 4 * 2;
    CHECK(indices.size() == (lakeTriangles + ribbonTriangles) * 3);

    // A ribbon whose nodes all coincide has no tangent anywhere: nothing
    // is emitted.
    FarWaterSet flat;
    FarWaterSet::Ribbon stuck;
    stuck.nodes = { { 5.0f, 5.0f, 1.0f, 2.0f }, { 5.0f, 5.0f, 1.0f, 2.0f } };
    flat.ribbons.push_back(stuck);
    vector<f32> noVerts;
    vector<u32> noIndices;
    render::terrain::buildFarWaterGeometry(flat, noVerts, noIndices);
    CHECK(noIndices.empty());
}

TEST_CASE("water geometry: mesh bytes are frozen (golden)") {
    // Golden hash over the raw vertex + index bytes of the local and far
    // builds. An INTENTIONAL mesher change re-captures it (print below);
    // an accidental one is what this test exists to catch. The raw-float
    // hash is sensitive to compiler FP codegen, so each platform /
    // toolchain keeps its OWN golden.
    const TerrainParams params;
    const WaterBodies bodies = makeBodies(params);
    vector<f32> verts;
    vector<u32> indices;
    render::terrain::buildLocalWaterGeometry(bodies, params, verts, indices);
    u64 h = hashMesh(verts, indices);
    vector<f32> farVerts;
    vector<u32> farIndices;
    render::terrain::buildFarWaterGeometry(makeFarSet(), farVerts,
                                           farIndices);
    h ^= hashMesh(farVerts, farIndices);
    h *= 1099511628211ull;
    MESSAGE("water geometry golden hash: " << h);
    const bool knownGolden = h == 10279103069440826151ull; // Windows x64 / MSVC
    CHECK(knownGolden);
}

TEST_CASE("water entry sources: the baked discharge boost scales the head") {
    const TerrainParams params;
    const WaterBodies bodies = makeBodies(params);
    render::terraingen::GridSpec spec;
    spec.texelSize = 2.0f;
    spec.n = 129; // 256 m window around the brook
    spec.originX = 150.0f;
    spec.originZ = 200.0f;
    render::terrain::WaterSimParams unit;
    unit.bakedDischargeBoost = 1.0f;
    render::terrain::WaterSimParams boosted;
    boosted.bakedDischargeBoost = 3.0f;
    const auto a = render::terrain::entrySourcesFromBodies(bodies, spec, {},
                                                           unit, 4.0f);
    const auto b = render::terrain::entrySourcesFromBodies(bodies, spec, {},
                                                           boosted, 4.0f);
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());
    // The true head inside the window is injected first.
    CHECK(a[0].x == doctest::Approx(200.0f));
    CHECK(b[0].discharge == doctest::Approx(3.0f * a[0].discharge));
}

TEST_CASE("terrain params: ground is final where the world says so") {
    TerrainParams story; // no sandbox identity: final everywhere
    CHECK(story.groundFinalAt(0.0f, 0.0f));
    TerrainParams sandbox;
    sandbox.sandbox = std::make_shared<render::SandboxTerrain>();
    CHECK(!sandbox.groundFinalAt(0.0f, 0.0f)); // no base published yet
    sandbox.base = std::make_shared<render::TerrainBase>();
    CHECK(!sandbox.groundFinalAt(0.0f, 0.0f)); // base with no region
}
