#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "world/terrain/TerrainRegions.hpp"
#include "world/worldspace/WorldForms.hpp"

// Baked-terrain base layer: absolute region grids under
// height() = base + detail + sculpt deltas. Null (or elsewhere) =
// BIT-IDENTICAL to the procedural noise — the same non-regression contract
// that protects HeightPatches.

namespace {

// A small constant-height region with hard edges (edgeBlend 0), exact by
// construction under Catmull-Rom.
render::TerrainRegion flatRegion(f32 originX, f32 originZ, f32 size,
                                 f32 texel, f32 level) {
    render::TerrainRegion region;
    region.originX = originX;
    region.originZ = originZ;
    region.texelSize = texel;
    region.edgeBlend = 0.0f;
    const u32 n = static_cast<u32>(size / texel) + 1u;
    region.width = n;
    region.height = n;
    region.heights.assign(static_cast<size_t>(n) * n, level);
    return region;
}

sptr<const render::TerrainBase> baseOf(render::TerrainRegion region) {
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(
        std::make_shared<render::TerrainRegion>(std::move(region)));
    return base;
}

} // namespace

TEST_CASE("contentTouchedSince scopes by rect and stays conservative") {
    render::TerrainParams params;
    // No bump yet: nothing touched.
    CHECK(!render::terrain::contentTouchedSince(params, 0, -1e9f, -1e9f,
                                                1e9f, 1e9f));
    // One publish at [0..4096]²: a window there sees it, a window at
    // 10 km does not.
    params.contentEvents.push(1, 0.0f, 0.0f, 4096.0f, 4096.0f);
    params.contentStamp = 1;
    CHECK(render::terrain::contentTouchedSince(params, 0, 1000.0f,
                                               1000.0f, 2000.0f,
                                               2000.0f));
    CHECK(!render::terrain::contentTouchedSince(params, 0, 10000.0f,
                                                10000.0f, 11000.0f,
                                                11000.0f));
    // Already seen: quiet.
    CHECK(!render::terrain::contentTouchedSince(params, 1, 1000.0f,
                                                1000.0f, 2000.0f,
                                                2000.0f));
    // Ring overflow: enough far-away events to evict stamp 1's entry —
    // a consumer that saw 0 must go conservative even though no kept
    // event intersects its window.
    for (u64 s = 2; s < 2 + render::TerrainParams::ContentEvents::kRing;
         ++s) {
        params.contentEvents.push(s, 50000.0f, 50000.0f, 51000.0f,
                                  51000.0f);
        params.contentStamp = s;
    }
    CHECK(render::terrain::contentTouchedSince(params, 0, 1000.0f,
                                               1000.0f, 2000.0f,
                                               2000.0f));
    // A consumer up to date past the eviction stays scoped: the kept
    // far events do not touch its window.
    CHECK(!render::terrain::contentTouchedSince(params, 2, 1000.0f,
                                                1000.0f, 2000.0f,
                                                2000.0f));
}

TEST_CASE("flow-free chunks may skip meshHeight: gate identity holds") {
    // A region whose flow mask is hot in one corner only. Where
    // flowMaskTouches says false, meshHeight at coarse spacing must be
    // bit-identical to height() — that identity is what lets the chunk
    // mesher probe once per chunk and call height() directly.
    render::TerrainRegion region =
        flatRegion(0.0f, 0.0f, 256.0f, 2.0f, 30.0f);
    // Sloped heights: if the 9-tap min DID fire, it would differ from
    // height() — a flat grid would hide a broken gate.
    for (u32 row = 0; row < region.height; ++row) {
        for (u32 col = 0; col < region.width; ++col) {
            region.heights[static_cast<size_t>(row) * region.width +
                           col] =
                30.0f + 0.5f * static_cast<f32>(col) +
                0.25f * static_cast<f32>(row);
        }
    }
    region.maskWidth = 17; // 16 m mask texels over 256 m
    region.maskHeight = 17;
    region.flow.assign(static_cast<size_t>(17) * 17, 0);
    region.flow[0] = 255; // hot texel at the (0, 0) corner only
    render::TerrainParams params;
    params.base = baseOf(std::move(region));

    // The hot corner is touched; the far quarter is not.
    CHECK(render::terrain::flowMaskTouches(params, 0.0f, 0.0f, 64.0f,
                                           64.0f));
    CHECK(!render::terrain::flowMaskTouches(params, 128.0f, 128.0f,
                                            192.0f, 192.0f));
    for (f32 z = 128.0f; z <= 192.0f; z += 5.3f) {
        for (f32 x = 128.0f; x <= 192.0f; x += 4.7f) {
            REQUIRE(render::terrain::meshHeight(params, x, z, 4.0f) ==
                    render::terrain::height(params, x, z)); // EXACT
        }
    }
    // Positive control: on the hot texel the gate DOES fire — the
    // min-tap digs below height() on this slope, so the equality above
    // is the gate staying silent, not a degenerate meshHeight.
    CHECK(render::terrain::meshHeight(params, 2.0f, 2.0f, 4.0f) <
          render::terrain::height(params, 2.0f, 2.0f));
}

TEST_CASE("region cell index is bit-identical to the linear scan") {
    // A 5x5 lattice of overlapping soft-edged regions (varied levels so
    // the blend accumulation order matters), sampled densely with and
    // without buildIndex(): heights and regionAt hits must match
    // BITWISE — the index only prunes provably non-covering regions and
    // keeps the ascending visit order.
    auto indexed = std::make_shared<render::TerrainBase>();
    for (i32 gz = 0; gz < 5; ++gz) {
        for (i32 gx = 0; gx < 5; ++gx) {
            render::TerrainRegion r = flatRegion(
                static_cast<f32>(gx) * 96.0f - 32.0f,
                static_cast<f32>(gz) * 96.0f - 32.0f, 128.0f, 2.0f,
                10.0f + static_cast<f32>(gz * 5 + gx) * 3.0f);
            r.edgeBlend = 48.0f; // soft rims: overlap bands everywhere
            indexed->regions.push_back(
                std::make_shared<render::TerrainRegion>(std::move(r)));
        }
    }
    auto linear = std::make_shared<render::TerrainBase>();
    linear->regions = indexed->regions;
    indexed->buildIndex();
    REQUIRE(!indexed->cellIndex.empty());
    render::TerrainParams a;
    a.base = indexed;
    render::TerrainParams b;
    b.base = linear;
    for (f32 z = -64.0f; z <= 512.0f; z += 13.7f) {
        for (f32 x = -64.0f; x <= 512.0f; x += 11.3f) {
            REQUIRE(render::terrain::height(a, x, z) ==
                    render::terrain::height(b, x, z)); // EXACT
            const render::TerrainRegion* ra = indexed->regionAt(x, z);
            const render::TerrainRegion* rb = linear->regionAt(x, z);
            REQUIRE((ra == nullptr) == (rb == nullptr));
            if (ra && rb) {
                // Same region by CONTENT identity (different vectors).
                REQUIRE(ra->originX == rb->originX);
                REQUIRE(ra->originZ == rb->originZ);
                REQUIRE(ra->heights[0] == rb->heights[0]);
            }
        }
    }
}

TEST_CASE("no baked base (or one elsewhere) is bit-identical to the noise") {
    render::TerrainParams pure;
    render::TerrainParams baked = pure;
    baked.base = baseOf(flatRegion(10000.0f, 10000.0f, 128.0f, 2.0f, 5.0f));
    for (const f32 x : { 0.0f, 31.7f, 1000.5f, -250.0f }) {
        for (const f32 z : { 0.0f, 359.2f, -64.0f }) {
            CHECK(render::terrain::height(pure, x, z) ==
                  render::terrain::height(baked, x, z)); // EXACT
        }
    }
}

TEST_CASE("inside a region the grid replaces the procedural base") {
    render::TerrainParams params;
    params.base = baseOf(flatRegion(0.0f, 0.0f, 128.0f, 2.0f, 42.0f));
    CHECK(render::terrain::height(params, 64.0f, 64.0f) ==
          doctest::Approx(42.0f));
    CHECK(render::terrain::height(params, 3.7f, 121.9f) ==
          doctest::Approx(42.0f));
    // Just outside: pure noise again.
    render::TerrainParams pure;
    CHECK(render::terrain::height(params, 130.0f, 64.0f) ==
          render::terrain::height(pure, 130.0f, 64.0f));
}

TEST_CASE("sculpt deltas stack on top of the baked base") {
    render::TerrainParams params;
    params.base = baseOf(flatRegion(0.0f, 0.0f, 128.0f, 2.0f, 42.0f));
    auto patches = std::make_shared<render::HeightPatches>();
    render::HeightPatch patch;
    patch.samples = 65;
    patch.deltas.assign(65 * 65, 3.0f); // chunk (0,0): x/z 0-64
    patches->chunks.emplace(render::HeightPatches::keyOf(0, 0),
                            std::move(patch));
    params.patches = patches;
    CHECK(render::terrain::height(params, 32.0f, 32.0f) ==
          doctest::Approx(45.0f));
    CHECK(render::terrain::height(params, 100.0f, 32.0f) ==
          doctest::Approx(42.0f)); // outside the sculpted chunk
}

TEST_CASE("edge blend is continuous across the region border") {
    render::TerrainParams params;
    auto region = flatRegion(0.0f, 0.0f, 512.0f, 2.0f, 80.0f);
    region.edgeBlend = 64.0f;
    params.base = baseOf(std::move(region));
    render::TerrainParams pure;

    // On the rim the blend weight is 0: baked == pure noise exactly
    // (mix at 0 returns its first operand).
    CHECK(render::terrain::height(params, 0.0f, 256.0f) ==
          render::terrain::height(pure, 0.0f, 256.0f));
    // Fully interior: the grid value.
    CHECK(render::terrain::height(params, 256.0f, 256.0f) ==
          doctest::Approx(80.0f));
    // Walking across the band never steps more than the local slope of
    // the two blended surfaces allows.
    f32 previous = render::terrain::height(params, -4.0f, 256.0f);
    for (f32 x = -3.5f; x <= 96.0f; x += 0.5f) {
        const f32 h = render::terrain::height(params, x, 256.0f);
        CHECK(std::abs(h - previous) < 2.0f);
        previous = h;
    }
}

TEST_CASE("a baked capture of the procedural terrain reproduces it") {
    render::TerrainParams pure;
    pure.seed = 1337;
    auto region = render::terrain::bakeProceduralRegion(pure, -64.0f, 320.0f,
                                                        128.0f, 2.0f);
    region.edgeBlend = 0.0f;
    CHECK(region.width == 65);
    CHECK(region.detailAmplitude == 0.0f);

    render::TerrainParams baked = pure;
    baked.base = baseOf(std::move(region));
    // At grid points Catmull-Rom passes through the samples.
    CHECK(render::terrain::height(baked, 0.0f, 384.0f) ==
          doctest::Approx(render::terrain::height(pure, 0.0f, 384.0f))
              .epsilon(0.0001));
    // Between texels the resample stays close (noise wavelengths >> 2 m).
    for (const f32 x : { -37.3f, 1.1f, 40.7f }) {
        for (const f32 z : { 333.9f, 371.4f, 440.2f }) {
            CHECK(render::terrain::height(baked, x, z) ==
                  doctest::Approx(render::terrain::height(pure, x, z))
                      .epsilon(0.02));
        }
    }
}

TEST_CASE(".trg round-trip preserves the region and its masks") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-trg";
    std::filesystem::create_directories(dir);
    const auto file = dir / "region.trg";

    auto region = flatRegion(-64.0f, 320.0f, 128.0f, 2.0f, 12.5f);
    region.edgeBlend = 48.0f;
    region.heights[100] = 99.0f;
    region.maskWidth = 33;
    region.maskHeight = 33;
    const size_t maskCells = 33 * 33;
    region.detailAmp.assign(maskCells, 200);
    region.flow.assign(maskCells, 10);
    region.wetness.assign(maskCells, 20);
    region.beach.assign(maskCells, 30);
    region.biome.assign(maskCells, 4);
    region.rockExposure.assign(maskCells, 40);
    region.waterWidth = 17;
    region.waterHeight = 17;
    region.waterTexel = 8.0f;
    const size_t wcells = 17 * 17;
    region.waterSurface.assign(wcells, 13.25f);
    region.waterDepth.assign(wcells, 24); // 0.75 m
    region.waterVelX.assign(wcells, -12);
    region.waterVelZ.assign(wcells, 7);
    region.waterFlux.assign(wcells, 90);

    REQUIRE(world::writeTrgFile(file, region));
    const auto back = world::readTrgFile(file);
    REQUIRE(back.has_value());
    CHECK(back->originX == region.originX);
    CHECK(back->originZ == region.originZ);
    CHECK(back->texelSize == region.texelSize);
    CHECK(back->edgeBlend == region.edgeBlend);
    CHECK(back->width == region.width);
    CHECK(back->heights == region.heights); // bit-exact: f32 pass-through
    CHECK(back->maskWidth == 33);
    CHECK(back->detailAmp == region.detailAmp);
    CHECK(back->biome == region.biome);
    CHECK(back->rockExposure == region.rockExposure);
    CHECK(back->waterWidth == 17);
    CHECK(back->waterTexel == 8.0f);
    CHECK(back->waterSurface == region.waterSurface); // bit-exact
    CHECK(back->waterDepth == region.waterDepth);
    CHECK(back->waterVelX == region.waterVelX);
    CHECK(back->waterVelZ == region.waterVelZ);
    CHECK(back->waterFlux == region.waterFlux);

    // Maskless, waterless round-trip.
    const auto bare = dir / "bare.trg";
    REQUIRE(world::writeTrgFile(bare,
                                flatRegion(0.0f, 0.0f, 64.0f, 2.0f, 1.0f)));
    const auto bareBack = world::readTrgFile(bare);
    REQUIRE(bareBack.has_value());
    CHECK(bareBack->maskWidth == 0);
    CHECK(bareBack->detailAmp.empty());
    CHECK(bareBack->waterWidth == 0);
    CHECK(bareBack->waterDepth.empty());
}

TEST_CASE(".trg rejects malformed files") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-trg";
    std::filesystem::create_directories(dir);

    const auto bogus = dir / "bogus.trg";
    {
        std::ofstream out { bogus, std::ios::binary };
        out << "NOPE and some trailing bytes";
    }
    CHECK(!world::readTrgFile(bogus).has_value());

    const auto truncated = dir / "truncated.trg";
    {
        auto region = flatRegion(0.0f, 0.0f, 64.0f, 2.0f, 1.0f);
        REQUIRE(world::writeTrgFile(truncated, region));
        std::filesystem::resize_file(truncated, 40);
    }
    CHECK(!world::readTrgFile(truncated).has_value());

    // Malformed in-memory region: refuse to write.
    render::TerrainRegion bad;
    bad.width = 8;
    bad.height = 8; // heights left empty
    CHECK(!world::writeTrgFile(dir / "bad.trg", bad));
}

TEST_CASE("TerrainRegionForm records build the baked base") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-trg";
    std::filesystem::create_directories(dir);
    REQUIRE(world::writeTrgFile(dir / "plateau.trg",
                                flatRegion(0.0f, 0.0f, 128.0f, 2.0f,
                                           64.0f)));

    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "88888888-8888-4888-8888-888888888899"
name = "terrain-region"

[[records]]
form = "80000000-0000-4000-8000-000000000011"
type = "TerrainRegionForm"
new = true
[records.fields]
displayName = "plateau"
asset = "80000000-0000-4000-8000-0000000000bb"
detailAmplitude = 2.5
detailWavelength = 40.0
detailOctaves = 2
)toml",
                                              types, "terrain-region");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    assets::AssetDatabase assetDb;
    assetDb.add(*core::Guid::fromString(
                    "80000000-0000-4000-8000-0000000000bb"),
                dir, "plateau.trg");

    const auto base = world::buildTerrainBase(db, assetDb);
    REQUIRE(base->regions.size() == 1);
    CHECK(base->regions[0]->detailAmplitude == doctest::Approx(2.5f));
    CHECK(base->regions[0]->detailOctaves == 2);

    render::TerrainParams params;
    params.base = base;
    // The Form's detail knobs are live: flat 64 m grid + detail noise
    // bounded by its amplitude, and actually present somewhere.
    f32 maxDeviation = 0.0f;
    for (f32 x = 8.0f; x <= 120.0f; x += 7.0f) {
        for (f32 z = 8.0f; z <= 120.0f; z += 7.0f) {
            const f32 h = render::terrain::height(params, x, z);
            CHECK(h > 64.0f - 2.6f);
            CHECK(h < 64.0f + 2.6f);
            maxDeviation = glm::max(maxDeviation, std::abs(h - 64.0f));
        }
    }
    CHECK(maxDeviation > 0.05f);
}
