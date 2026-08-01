#include <doctest/doctest.h>

#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "world/terrain/WaterBodiesBuilder.hpp"
#include "world/worldspace/WorldForms.hpp"

// Water bodies: the swim/wading queries and the Forms -> WaterBodies
// builder (sea + altitude lakes + river ribbons).

using render::LakeSurface;
using render::RiverNode;
using render::RiverSurface;
using render::WaterBodies;
using render::terrain::waterDepthAt;
using render::terrain::waterSurfaceAt;

namespace {

WaterBodies testBodies() {
    WaterBodies bodies;
    bodies.seaLevel = 21.0f;
    LakeSurface lake;
    lake.level = 130.0f;
    lake.minX = 100.0f;
    lake.maxX = 300.0f;
    lake.minZ = -50.0f;
    lake.maxZ = 150.0f;
    bodies.lakes.push_back(lake);
    RiverSurface river;
    river.nodes = { { 500.0f, 0.0f, 80.0f, 4.0f },
                    { 500.0f, 100.0f, 70.0f, 6.0f } };
    river.minX = 490.0f;
    river.maxX = 510.0f;
    river.minZ = -10.0f;
    river.maxZ = 110.0f;
    bodies.rivers.push_back(river);
    return bodies;
}

} // namespace

TEST_CASE("water queries: sea fallback, lakes gated by plausibility") {
    const WaterBodies bodies = testBodies();
    // Open sea: probe near the surface swims, a mountain top does not.
    CHECK(waterSurfaceAt(bodies, -1000.0f, 0.0f, 20.0f) ==
          doctest::Approx(21.0f));
    CHECK(!waterSurfaceAt(bodies, -1000.0f, 0.0f, 200.0f).has_value());
    // Inside the lake bbox at lake height: the lake wins over the sea.
    CHECK(waterSurfaceAt(bodies, 200.0f, 50.0f, 129.0f) ==
          doctest::Approx(130.0f));
    // Under the lake, at valley floor: NOT in the lake (a probe 100 m
    // below its surface), and too high for the sea.
    CHECK(!waterSurfaceAt(bodies, 200.0f, 50.0f, 30.0f).has_value());
    // Hovering high above the lake: dry.
    CHECK(!waterSurfaceAt(bodies, 200.0f, 50.0f, 170.0f).has_value());
}

TEST_CASE("a masked lake only exists over its basin cells") {
    WaterBodies bodies;
    bodies.seaLevel = 21.0f;
    LakeSurface lake;
    lake.level = 130.0f;
    lake.minX = 0.0f;
    lake.maxX = 80.0f;
    lake.minZ = 0.0f;
    lake.maxZ = 80.0f;
    lake.maskWidth = 11;
    lake.maskHeight = 11;
    lake.maskTexel = 8.0f;
    lake.mask.assign(11 * 11, 0);
    // West half is water, east half is not (a valley under the bbox).
    for (u32 row = 0; row < 11; ++row) {
        for (u32 col = 0; col <= 5; ++col) {
            lake.mask[row * 11 + col] = 1;
        }
    }
    bodies.lakes.push_back(lake);
    CHECK(waterSurfaceAt(bodies, 16.0f, 40.0f, 129.0f) ==
          doctest::Approx(130.0f));
    // Same bbox, outside the basin: dry — no floating sheet.
    CHECK(!waterSurfaceAt(bodies, 72.0f, 40.0f, 129.0f).has_value());
}

TEST_CASE("water queries: river ribbon coverage and interpolation") {
    const WaterBodies bodies = testBodies();
    // Mid-course, on the centerline: surface interpolates 80 -> 70.
    const auto mid = waterSurfaceAt(bodies, 500.0f, 50.0f, 74.0f);
    REQUIRE(mid.has_value());
    CHECK(*mid == doctest::Approx(75.0f).epsilon(0.01));
    // Off the ribbon (widths 4-6 m): dry at that height.
    CHECK(!waterSurfaceAt(bodies, 520.0f, 50.0f, 74.0f).has_value());
    // Depth: terrain 2 m under the surface -> 2 m of water.
    CHECK(waterDepthAt(bodies, 500.0f, 50.0f, 73.0f) ==
          doctest::Approx(2.0f).epsilon(0.01));
    CHECK(waterDepthAt(bodies, 200.0f, 50.0f, 126.0f) ==
          doctest::Approx(4.0f).epsilon(0.01));
}

TEST_CASE("WaterBodyForm / RiverForm records build WaterBodies") {
    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "88888888-8888-4888-8888-8888888888aa"
name = "water"

[[records]]
form = "80000000-0000-4000-8000-000000000021"
type = "WaterBodyForm"
new = true
[records.fields]
displayName = "mountain lake"
surfaceLevel = 142.5
minX = 10.0
minZ = 20.0
maxX = 90.0
maxZ = 120.0

[[records]]
form = "80000000-0000-4000-8000-000000000022"
type = "RiverForm"
new = true
[records.fields]
displayName = "creek"
flowSpeed = 1.5

[[records]]
form = "80000000-0000-4000-8000-000000000023"
type = "RiverPointForm"
new = true
[records.fields]
parent = "80000000-0000-4000-8000-000000000022"
index = 1
position = [0.0, 60.0, 100.0]
halfWidth = 3.0

[[records]]
form = "80000000-0000-4000-8000-000000000024"
type = "RiverPointForm"
new = true
[records.fields]
parent = "80000000-0000-4000-8000-000000000022"
index = 0
position = [0.0, 66.0, 0.0]
halfWidth = 2.0
)toml",
                                              types, "water");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    const auto bodies = world::buildWaterBodies(db, 21.0f);
    CHECK(bodies->seaLevel == doctest::Approx(21.0f));
    REQUIRE(bodies->lakes.size() == 1);
    CHECK(bodies->lakes[0].level == doctest::Approx(142.5f));
    REQUIRE(bodies->rivers.size() == 1);
    const RiverSurface& river = bodies->rivers[0];
    REQUIRE(river.nodes.size() == 2);
    // Sorted by index: node 0 first (surface 66 upstream).
    CHECK(river.nodes[0].surface == doctest::Approx(66.0f));
    CHECK(river.nodes[1].surface == doctest::Approx(60.0f));
    CHECK(river.flowSpeed == doctest::Approx(1.5f));
    // The lake resolves through the query.
    CHECK(waterSurfaceAt(*bodies, 50.0f, 70.0f, 141.0f) ==
          doctest::Approx(142.5f));
}

TEST_CASE("waterFlowAt: downstream, faster mid-channel, still elsewhere") {
    const WaterBodies bodies = testBodies();
    // The test river runs +z (500,0) -> (500,100), speed default 1.0.
    const Vec2 mid =
        render::terrain::waterFlowAt(bodies, 500.0f, 50.0f, 75.0f);
    CHECK(mid.y > 0.8f); // downstream +z, near full speed mid-channel
    CHECK(std::abs(mid.x) < 1.0e-4f);
    const Vec2 bank =
        render::terrain::waterFlowAt(bodies, 504.5f, 50.0f, 75.0f);
    CHECK(bank.y > 0.0f);
    CHECK(bank.y < mid.y); // the banks drag

    // Dry land, a still lake, and a probe far above: all zero.
    const Vec2 zero { 0.0f, 0.0f };
    CHECK(render::terrain::waterFlowAt(bodies, 0.0f, 0.0f, 10.0f) ==
          zero);
    CHECK(render::terrain::waterFlowAt(bodies, 200.0f, 50.0f, 130.0f) ==
          zero);
    CHECK(render::terrain::waterFlowAt(bodies, 500.0f, 50.0f, 500.0f) ==
          zero);
}

TEST_CASE("waterFlowAt blends overlapping rivers continuously") {
    WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    RiverSurface east; // flows +x
    east.nodes = { { 0.0f, 0.0f, 10.0f, 5.0f },
                   { 100.0f, 0.0f, 9.0f, 5.0f } };
    east.minX = -5.0f;
    east.maxX = 105.0f;
    east.minZ = -5.0f;
    east.maxZ = 5.0f;
    east.flowSpeed = 2.0f;
    RiverSurface north = east; // same footprint, flows +z
    north.nodes = { { 50.0f, -5.0f, 10.0f, 5.0f },
                    { 50.0f, 5.0f, 9.5f, 5.0f } };
    north.minZ = -10.0f;
    north.maxZ = 10.0f;
    bodies.rivers.push_back(east);
    bodies.rivers.push_back(north);

    // On the junction both contribute: the blend carries both
    // directions instead of snapping to one ribbon.
    const Vec2 blended =
        render::terrain::waterFlowAt(bodies, 50.0f, 0.0f, 10.0f);
    CHECK(blended.x > 0.3f);
    CHECK(blended.y > 0.3f);
}

TEST_CASE("WaterMaterialForm presets resolve to stable indices") {
    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "88888888-8888-4888-8888-8888888888bb"
name = "lava"

[[records]]
form = "80000000-0000-4000-8000-000000000031"
type = "WaterMaterialForm"
new = true
[records.fields]
displayName = "lava"
emissiveColor = [1.0, 0.35, 0.05]
emissiveStrength = 3.0
viscosity = 0.8
flowSpeedScale = 0.3

[[records]]
form = "80000000-0000-4000-8000-000000000032"
type = "WaterBodyForm"
new = true
[records.fields]
displayName = "lava pool"
surfaceLevel = 50.0
minX = 0.0
minZ = 0.0
maxX = 40.0
maxZ = 40.0
material = "80000000-0000-4000-8000-000000000031"

[[records]]
form = "80000000-0000-4000-8000-000000000033"
type = "WaterBodyForm"
new = true
[records.fields]
displayName = "plain pond"
surfaceLevel = 30.0
minX = 100.0
minZ = 0.0
maxX = 140.0
maxZ = 40.0

[[records]]
form = "80000000-0000-4000-8000-000000000034"
type = "WaterBodyForm"
new = true
[records.fields]
displayName = "dangling"
surfaceLevel = 20.0
minX = 200.0
minZ = 0.0
maxX = 240.0
maxZ = 40.0
material = "80000000-0000-4000-8000-0000000000ff"
)toml",
                                              types, "lava");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    const auto bodies = world::buildWaterBodies(db, 21.0f);
    // Slot 0 = default water; the lava preset landed after it.
    REQUIRE(bodies->materials.size() == 2);
    CHECK(bodies->materials[0].emissiveStrength == 0.0f);
    CHECK(bodies->materials[1].emissiveStrength ==
          doctest::Approx(3.0f));
    CHECK(bodies->materials[1].viscosity == doctest::Approx(0.8f));
    REQUIRE(bodies->lakes.size() == 3);
    CHECK(bodies->lakes[0].materialIndex == 1); // lava pool
    CHECK(bodies->lakes[1].materialIndex == 0); // plain pond
    CHECK(bodies->lakes[2].materialIndex == 0); // dangling -> default
}
