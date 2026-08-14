#include <doctest/doctest.h>

#include "engine/dungeon/DensityField.hpp"

using namespace dungeon;

namespace {

SpaceGraph makeSpace(u32 seed) {
    MissionParams mp;
    mp.seed = seed;
    mp.subCycles = 1;
    const MissionGraph mission = buildMissionGraph(mp);
    SpaceParams sp;
    sp.seed = seed;
    sp.gridX = 7;
    sp.gridZ = 7;
    sp.floors = 2;
    return buildSpaceGraph(mission, sp);
}

} // namespace

TEST_CASE("dungeon density: room centers are air, deep rock stays solid") {
    const SpaceGraph space = makeSpace(4);
    REQUIRE_FALSE(space.rooms.empty());
    const DensityField field(space, DensityParams { 4 });

    for (u32 r = 0; r < space.rooms.size(); ++r) {
        const Vec3 c = roomCenter(space, r) +
                       Vec3 { 0.0f, 2.0f, 0.0f }; // inside the carved belly
        CAPTURE(r);
        CHECK(field.sample(c) < 0.0f);
    }
    // Far outside the carved bounds: solid rock.
    CHECK(field.sample(field.boundsMax() + Vec3 { 20.0f }) > 0.0f);
    CHECK(field.sample(field.boundsMin() - Vec3 { 20.0f }) > 0.0f);
}

TEST_CASE("dungeon density: corridor waypoints are carved through") {
    const SpaceGraph space = makeSpace(12);
    REQUIRE_FALSE(space.rooms.empty());
    DensityParams dp;
    dp.seed = 12;
    const DensityField field(space, dp);
    for (const SpaceEdge& e : space.edges) {
        for (const GridPos& g : e.path) {
            const Vec3 p = slotCenter(space.params, g) +
                           Vec3 { 0.0f, dp.tunnelRadius * 0.45f, 0.0f };
            CAPTURE(g.x);
            CAPTURE(g.z);
            CAPTURE(g.floor);
            CHECK(field.sample(p) < 0.0f);
        }
    }
}

TEST_CASE("dungeon density: sampling is pure and deterministic") {
    const SpaceGraph space = makeSpace(7);
    const DensityField a(space, DensityParams { 7 });
    const DensityField b(space, DensityParams { 7 });
    const Vec3 probe = roomCenter(space, space.goal) + Vec3 { 1.3f, 0.7f, -2.1f };
    CHECK(a.sample(probe) == b.sample(probe));
    CHECK(a.sample(probe) == a.sample(probe));

    DensityParams other;
    other.seed = 8;
    const DensityField c(space, other);
    // Same geometry, different seed: the noise (thus the surface) moves.
    // Probe at mid-height across the room span: floor planes are seedless
    // by design, walls and ceilings carry the noise.
    bool differs = false;
    for (i32 i = 0; i < 20 && !differs; ++i) {
        const Vec3 p = roomCenter(space, space.entrance) +
                       Vec3 { static_cast<f32>(i) * 0.9f, 2.5f, 0.53f };
        differs = a.sample(p) != c.sample(p);
    }
    CHECK(differs);
}

TEST_CASE("dungeon density: density rises from air into rock") {
    const SpaceGraph space = makeSpace(4);
    const DensityField field(space, DensityParams { 4 });
    // March upward from just above a room floor (the floor plane itself is
    // the d = 0 isosurface): past the ceiling the density keeps rising.
    const Vec3 c = roomCenter(space, space.entrance) + Vec3 { 0, 1, 0 };
    Vec3 p = c;
    while (field.sample(p) < 0.0f && p.y < c.y + 30.0f) {
        p.y += 0.5f;
    }
    CHECK(field.sample({ p.x, p.y + 0.25f, p.z }) >
          field.sample({ p.x, p.y - 0.25f, p.z }));
}
