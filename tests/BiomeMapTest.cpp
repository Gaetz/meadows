#include <doctest/doctest.h>

#include <filesystem>

#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "world/terrain/BiomeMapBuilder.hpp"
#include "world/worldspace/WorldForms.hpp"

// Biomes: id resolution (region mask / painted map / neutral) and the
// biome-aware material weights. The neutral biome MUST reproduce the
// legacy rules exactly — that is what keeps existing scatter unmoved.

TEST_CASE("neutral biome weights equal the legacy weights") {
    render::TerrainParams params; // no biomes set
    for (const f32 x : { 0.0f, 400.0f }) {
        for (const f32 h : { 10.0f, 60.0f, 170.0f }) {
            const Vec3 n =
                glm::normalize(Vec3 { 0.2f, 1.0f, 0.1f });
            const auto legacy =
                render::terrain::materialWeights(params, h, n);
            const auto biome = render::terrain::materialWeightsAt(
                params, x, x, h, n);
            CHECK(biome.rock == legacy.rock);
            CHECK(biome.snow == legacy.snow);
            CHECK(biome.sand == legacy.sand);
            CHECK(biome.grass == legacy.grass);
        }
    }
}

TEST_CASE("BiomeForm records build the table; the painted map resolves") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-tbm";
    std::filesystem::create_directories(dir);
    world::BiomeIndexMap map;
    map.originX = 0.0f;
    map.originZ = 0.0f;
    map.texelSize = 16.0f;
    map.width = 8;
    map.height = 8;
    map.indices.assign(64, 0);
    for (u32 i = 0; i < 32; ++i) {
        map.indices[i] = 3; // north half: tundra
    }
    REQUIRE(world::writeTbmFile(dir / "biomes.tbm", map));
    const auto back = world::readTbmFile(dir / "biomes.tbm");
    REQUIRE(back.has_value());
    CHECK(back->indices == map.indices);

    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "88888888-8888-4888-8888-8888888888bb"
name = "biomes"

[[records]]
form = "80000000-0000-4000-8000-000000000031"
type = "BiomeForm"
new = true
[records.fields]
displayName = "Tundra"
paletteIndex = 3
snowLineOffset = -80.0
grassPresence = 0.4
temperature = -0.8

[[records]]
form = "80000000-0000-4000-8000-000000000032"
type = "BiomeMapForm"
new = true
[records.fields]
asset = "80000000-0000-4000-8000-0000000000cc"
)toml",
                                              types, "biomes");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    assets::AssetDatabase assetDb;
    assetDb.add(*core::Guid::fromString(
                    "80000000-0000-4000-8000-0000000000cc"),
                dir, "biomes.tbm");

    render::TerrainParams params;
    params.biomes = world::buildBiomeSet(db, assetDb);
    REQUIRE(params.biomes->table.size() == 4);

    // North half (z < 8 rows * 16 m... rows along +Z: indices[0..31] are
    // z rows 0-3): tundra; south half neutral.
    const render::BiomeParams& north =
        render::terrain::biomeAt(params, 40.0f, 16.0f);
    CHECK(north.temperature == doctest::Approx(-0.8f));
    const render::BiomeParams& south =
        render::terrain::biomeAt(params, 40.0f, 100.0f);
    CHECK(south.temperature == doctest::Approx(0.0f));

    // The shifted snow line turns a 100 m hilltop snowy in tundra only.
    const Vec3 up { 0.0f, 1.0f, 0.0f };
    const auto snowy = render::terrain::materialWeightsAt(
        params, 40.0f, 16.0f, 130.0f, up);
    const auto grassy = render::terrain::materialWeightsAt(
        params, 40.0f, 100.0f, 130.0f, up);
    CHECK(snowy.snow > 0.5f);
    CHECK(grassy.snow < 0.1f);
    // Grass presence multiplier bites.
    CHECK(grassy.grass > snowy.grass);
}
