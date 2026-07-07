#include <doctest/doctest.h>

#include <filesystem>

#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "world/terrain/TerrainPatches.hpp"
#include "world/worldspace/WorldForms.hpp"

// Chantier 2 B8: authored terrain = procedural noise + per-chunk delta
// grids (assets). The overlay rides inside TerrainParams; null or empty =
// BIT-IDENTICAL to the pure noise (the non-regression contract protecting
// the whole existing landscape).

namespace {

render::HeightPatch flatPatch(u32 samples, f32 delta) {
    render::HeightPatch patch;
    patch.samples = samples;
    patch.deltas.assign(static_cast<size_t>(samples) * samples, delta);
    return patch;
}

} // namespace

TEST_CASE("no overlay (or an empty one) is bit-identical to the noise") {
    render::TerrainParams pure;
    render::TerrainParams overlaid = pure;
    overlaid.patches = std::make_shared<render::HeightPatches>();
    for (const f32 x : { 0.0f, 31.7f, 1000.5f, -250.0f }) {
        for (const f32 z : { 0.0f, 359.2f, -64.0f }) {
            CHECK(render::terrain::height(pure, x, z) ==
                  render::terrain::height(overlaid, x, z)); // EXACT
        }
    }
}

TEST_CASE("a chunk delta applies bilinearly, only inside its chunk") {
    render::TerrainParams params;
    auto patchesPtr = std::make_shared<render::HeightPatches>();
    render::HeightPatches& patches = *patchesPtr;
    // Chunk (1, 5): x 64-128, z 320-384, uniform +10 m.
    patches.chunks.emplace(render::HeightPatches::keyOf(1, 5),
                           flatPatch(65, 10.0f));
    render::TerrainParams overlaid = params;
    overlaid.patches = patchesPtr;

    CHECK(render::terrain::height(overlaid, 96.0f, 350.0f) ==
          doctest::Approx(render::terrain::height(params, 96.0f, 350.0f) +
                          10.0f));
    // Outside the authored chunk: untouched.
    CHECK(render::terrain::height(overlaid, 30.0f, 350.0f) ==
          render::terrain::height(params, 30.0f, 350.0f));

    // A gradient grid interpolates: delta 0 at the west edge, 8 at the
    // east edge, 4 in the middle.
    render::HeightPatch ramp;
    ramp.samples = 65;
    ramp.deltas.resize(65 * 65);
    for (u32 row = 0; row < 65; ++row) {
        for (u32 col = 0; col < 65; ++col) {
            ramp.deltas[row * 65 + col] =
                8.0f * static_cast<f32>(col) / 64.0f;
        }
    }
    patches.chunks[render::HeightPatches::keyOf(1, 5)] = ramp;
    const f32 base = render::terrain::height(params, 96.0f, 350.0f);
    CHECK(render::terrain::height(overlaid, 96.0f, 350.0f) ==
          doctest::Approx(base + 4.0f).epsilon(0.001));
}

TEST_CASE("shared edge samples keep chunk borders seamless") {
    render::TerrainParams params;
    auto patchesPtr = std::make_shared<render::HeightPatches>();
    render::HeightPatches& patches = *patchesPtr;
    // Both chunks carry the SAME value on their shared edge (x = 128):
    // col 64 of chunk 1 and col 0 of chunk 2.
    patches.chunks.emplace(render::HeightPatches::keyOf(1, 5),
                           flatPatch(65, 6.0f));
    patches.chunks.emplace(render::HeightPatches::keyOf(2, 5),
                           flatPatch(65, 6.0f));
    render::TerrainParams overlaid = params;
    overlaid.patches = patchesPtr;
    const f32 left = render::terrain::height(overlaid, 127.999f, 350.0f);
    const f32 right = render::terrain::height(overlaid, 128.001f, 350.0f);
    CHECK(left == doctest::Approx(right).epsilon(0.001));
}

TEST_CASE("the village site level is what village.toml was authored for") {
    // The house modules are placed at ABSOLUTE heights (snapToGround =
    // false): this pins the terrain level under them at seed 1337. If the
    // noise or seed ever changes, this fails BEFORE the village floats.
    // Values recorded 2026-07-06; the village pad levels this to 134.6 m.
    render::TerrainParams params; // demo defaults, seed 1337
    CHECK(render::terrain::height(params, 46.0f, 356.0f) ==
          doctest::Approx(133.537f).epsilon(0.001));
    CHECK(render::terrain::height(params, 58.0f, 356.0f) ==
          doctest::Approx(135.435f).epsilon(0.001));
    CHECK(render::terrain::height(params, 46.0f, 368.0f) ==
          doctest::Approx(133.281f).epsilon(0.001));
    CHECK(render::terrain::height(params, 58.0f, 368.0f) ==
          doctest::Approx(135.884f).epsilon(0.001));
}

TEST_CASE(".ter round-trip and Forms -> overlay build") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-ter";
    std::filesystem::create_directories(dir);
    const auto file = dir / "patch_1_5.ter";
    REQUIRE(world::writeTerFile(file, flatPatch(33, 2.5f)));
    const auto back = world::readTerFile(file);
    REQUIRE(back.has_value());
    CHECK(back->samples == 33);
    CHECK(back->deltas[100] == doctest::Approx(2.5f));

    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "88888888-8888-4888-8888-888888888888"
name = "terrain"

[[records]]
form = "80000000-0000-4000-8000-000000000001"
type = "TerrainPatchForm"
new = true
[records.fields]
chunkX = 1
chunkZ = 5
asset = "80000000-0000-4000-8000-0000000000aa"
)toml",
                                              types, "terrain");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    assets::AssetDatabase assetDb;
    assetDb.add(*core::Guid::fromString(
                    "80000000-0000-4000-8000-0000000000aa"),
                dir, "patch_1_5.ter");

    const auto overlay = world::buildHeightPatches(db, assetDb);
    REQUIRE(overlay->chunks.size() == 1);
    render::TerrainParams params;
    render::TerrainParams overlaid = params;
    overlaid.patches = overlay;
    CHECK(render::terrain::height(overlaid, 96.0f, 350.0f) ==
          doctest::Approx(render::terrain::height(params, 96.0f, 350.0f) +
                          2.5f));
}
