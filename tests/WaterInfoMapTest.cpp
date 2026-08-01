#include <doctest/doctest.h>

#include <cmath>

#include "engine/terrain/WaterInfoMap.hpp"

// The camera-local water-info bake: per-texel surface/depth/flow with
// per-pixel junction blending — the data the water shader composites
// against.

using namespace render;
using render::terrain::bakeWaterInfo;
using render::terrain::kWaterInfoDry;
using render::terrain::WaterInfoMap;

namespace {

constexpr u32 kSize = 128;
constexpr f32 kSpan = 256.0f; // 2 m texels

size_t texelAt(const WaterInfoMap& map, f32 x, f32 z) {
    const f32 texel = map.span / static_cast<f32>(map.size);
    const f32 minX = map.center.x - map.span * 0.5f;
    const f32 minZ = map.center.y - map.span * 0.5f;
    const u32 col = static_cast<u32>((x - minX) / texel);
    const u32 row = static_cast<u32>((z - minZ) / texel);
    return static_cast<size_t>(row) * map.size + col;
}

RiverSurface makeRiver(Vec2 from, Vec2 to, f32 surface, f32 half,
                       f32 speed) {
    RiverSurface river;
    river.nodes = { { from.x, from.y, surface, half },
                    { (from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f,
                      surface - 0.2f, half },
                    { to.x, to.y, surface - 0.4f, half } };
    river.minX = glm::min(from.x, to.x) - half - 2.0f;
    river.maxX = glm::max(from.x, to.x) + half + 2.0f;
    river.minZ = glm::min(from.y, to.y) - half - 2.0f;
    river.maxZ = glm::max(from.y, to.y) + half + 2.0f;
    river.flowSpeed = speed;
    return river;
}

} // namespace

TEST_CASE("water-info bake: lake and river channels, dry sentinel") {
    WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    LakeSurface lake;
    lake.level = 30.0f;
    lake.minX = -100.0f;
    lake.maxX = -40.0f;
    lake.minZ = -100.0f;
    lake.maxZ = -40.0f;
    bodies.lakes.push_back(lake);
    bodies.rivers.push_back(
        makeRiver({ 0.0f, -80.0f }, { 0.0f, 80.0f }, 20.0f, 6.0f, 2.0f));

    const auto flat = [](f32, f32) { return 18.0f; };
    const WaterInfoMap map =
        bakeWaterInfo(bodies, { 0.0f, 0.0f }, kSpan, kSize, flat);
    const WaterInfoMap map2 =
        bakeWaterInfo(bodies, { 0.0f, 0.0f }, kSpan, kSize, flat);
    CHECK(map.surface == map2.surface); // deterministic, bit-exact
    CHECK(map.flow == map2.flow);

    // Lake texel: its level, depth vs the ground, zero flow.
    const size_t inLake = texelAt(map, -70.0f, -70.0f);
    CHECK(map.surface[inLake] == doctest::Approx(30.0f));
    CHECK(map.depth[inLake] == doctest::Approx(12.0f));
    CHECK(map.flow[inLake].x == 0.0f);
    // River mid-channel: surface, downstream (+z) flow near full speed.
    const size_t midRiver = texelAt(map, 0.0f, 0.0f);
    CHECK(map.surface[midRiver] > 19.0f);
    CHECK(map.flow[midRiver].y > 1.5f);
    CHECK(std::abs(map.flow[midRiver].x) < 0.15f);
    // Dry land: sentinel, no flow, no depth.
    const size_t dry = texelAt(map, 90.0f, 90.0f);
    CHECK(map.surface[dry] == kWaterInfoDry);
    CHECK(map.depth[dry] == 0.0f);
}

TEST_CASE("two crossing rivers blend their flow continuously") {
    WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    bodies.rivers.push_back(
        makeRiver({ -80.0f, 0.0f }, { 80.0f, 0.0f }, 20.0f, 8.0f, 2.0f));
    bodies.rivers.push_back(
        makeRiver({ 0.0f, -80.0f }, { 0.0f, 80.0f }, 20.0f, 8.0f, 2.0f));
    const auto flat = [](f32, f32) { return 17.0f; };
    const WaterInfoMap map =
        bakeWaterInfo(bodies, { 0.0f, 0.0f }, kSpan, kSize, flat);

    // At the crossing both currents contribute.
    const Vec2 atCross = map.flow[texelAt(map, 0.0f, 0.0f)];
    CHECK(atCross.x > 0.4f);
    CHECK(atCross.y > 0.4f);
    // Away from the junction each arm keeps its own direction.
    const Vec2 east = map.flow[texelAt(map, 60.0f, 0.0f)];
    CHECK(east.x > 1.0f);
    CHECK(std::abs(east.y) < 0.2f);
    // Walking across the junction the flow changes CONTINUOUSLY: no
    // texel-to-texel jump larger than the blend can explain.
    f32 maxJump = 0.0f;
    for (f32 x = -30.0f; x < 30.0f; x += 2.0f) {
        const Vec2 a = map.flow[texelAt(map, x, 0.0f)];
        const Vec2 b = map.flow[texelAt(map, x + 2.0f, 0.0f)];
        maxJump = std::max(maxJump, glm::length(b - a));
    }
    CHECK(maxJump < 1.0f);
}

TEST_CASE("floating water never reports negative depth") {
    // A lake whose ground pokes ABOVE its level on one side: depth
    // clamps to zero instead of going negative (the reconcile pass owns
    // hiding such cells; the map must stay sane regardless).
    WaterBodies bodies;
    bodies.seaLevel = 0.0f;
    LakeSurface lake;
    lake.level = 10.0f;
    lake.minX = -20.0f;
    lake.maxX = 20.0f;
    lake.minZ = -20.0f;
    lake.maxZ = 20.0f;
    bodies.lakes.push_back(lake);
    const auto ridge = [](f32 x, f32) {
        return x > 0.0f ? 15.0f : 5.0f;
    };
    const WaterInfoMap map =
        bakeWaterInfo(bodies, { 0.0f, 0.0f }, kSpan, kSize, ridge);
    CHECK(map.depth[texelAt(map, -10.0f, 0.0f)] == doctest::Approx(5.0f));
    CHECK(map.depth[texelAt(map, 10.0f, 0.0f)] == 0.0f);
}
