#include <doctest/doctest.h>

#include "engine/core/Jobs.hpp"
#include "engine/terrain/SandboxTerrain.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/TerrainNoise.hpp"
#include "engine/terrain/WaterBodies.hpp"
#include "engine/terrain/WaterSimControl.hpp"

// The sim window's phase machine, driven headless: no device, no
// renderer — the control decides, kicks its worker and hands GPU
// requests to whoever draws. A tiny window (17 x 17 texels) keeps the
// pre-roll it kicks in the millisecond range; the tests assert on the
// phase right after the kick and never wait for the landing.

namespace {

using render::LakeSurface;
using render::TerrainParams;
using render::WaterBodies;
using render::terrain::WaterSimControl;
using SimPhase = WaterSimControl::SimPhase;
using Kind = WaterSimControl::GpuRequest::Kind;

// A maskless pond (the bbox-quad path) near the camera: the pre-roll
// then also runs its pin + settle burst.
sptr<const WaterBodies> makeBodies(const TerrainParams& params) {
    auto bodies = std::make_shared<WaterBodies>();
    LakeSurface pond;
    pond.minX = 4.0f;
    pond.minZ = 4.0f;
    pond.maxX = 12.0f;
    pond.maxZ = 10.0f;
    pond.level = render::terrain::height(params, 8.0f, 7.0f) + 0.5f;
    bodies->lakes.push_back(pond);
    return bodies;
}

// The authored (story) world: no sandbox identity, a base so the
// policy accepts the terrain — ground final everywhere.
TerrainParams makeStoryParams() {
    TerrainParams params;
    params.base = std::make_shared<render::TerrainBase>();
    return params;
}

// Small window: span 32 m at 2 m texels -> n = 17.
void configureTiny(WaterSimControl& sim) {
    sim.simConfig().span = 32.0f;
    sim.simConfig().texel = 2.0f;
}

WaterSimControl::Inputs frameAt(const TerrainParams& params,
                                const sptr<const WaterBodies>& bodies) {
    return { .params = &params,
             .bodies = bodies,
             .cameraPos = { 8.0f, 50.0f, 8.0f },
             .dt = 1.0f / 60.0f };
}

} // namespace

TEST_CASE("water sim control: a final ground enters through the pre-roll") {
    core::JobSystem jobs { 1 };
    WaterSimControl sim;
    sim.create(jobs);
    configureTiny(sim);
    const TerrainParams params = makeStoryParams();
    const auto bodies = makeBodies(params);

    CHECK(sim.phase() == SimPhase::Off);
    CHECK(!sim.simIsValid());
    CHECK(!sim.simIsPreRolling());
    const u32 epochBefore = sim.epoch();

    const auto& requests = sim.update(frameAt(params, bodies));
    CHECK(requests.empty()); // nothing landed yet: nothing to upload
    CHECK(sim.phase() == SimPhase::PreRolling);
    CHECK(sim.simIsPreRolling());
    CHECK(!sim.simIsSettling());
    CHECK(!sim.simIsLive());
    CHECK(!sim.simIsValid()); // no snapshot before the solve lands
    CHECK(sim.simMapInfo().w == 0.0f); // the shaders see "no sim"
    CHECK(!sim.simWindowIntersects(0.0f, 0.0f, 100.0f, 100.0f));
    CHECK(sim.epoch() == epochBefore); // entering a phase never bumps

    // Dropping the window: Off, and the epoch moves so the in-flight
    // solve dies on arrival (cached, never landed as current).
    sim.simInvalidate("test");
    CHECK(sim.phase() == SimPhase::Off);
    CHECK(sim.epoch() == epochBefore + 1);
    CHECK(!sim.simIsPreRolling());
    CHECK(!sim.simIsValid());

    sim.destroy(); // orphans the lane before the job system joins
    CHECK(sim.phase() == SimPhase::Off);
}

TEST_CASE("water sim control: an unpublished sandbox tile waits for ground") {
    core::JobSystem jobs { 1 };
    WaterSimControl sim;
    sim.create(jobs);
    configureTiny(sim);
    TerrainParams params = makeStoryParams();
    params.sandbox = std::make_shared<render::SandboxTerrain>();
    const auto bodies = makeBodies(params);
    // Sandbox identity + a base with no region under the camera: the
    // ground is not final, no pre-roll is kicked.
    REQUIRE(!params.groundFinalAt(8.0f, 8.0f));

    sim.update(frameAt(params, bodies));
    CHECK(sim.phase() == SimPhase::AwaitingGround);
    CHECK(!sim.simIsPreRolling());
    CHECK(!sim.simIsValid());
    // Staying put keeps waiting (no transition, no epoch move).
    const u32 epoch = sim.epoch();
    sim.update(frameAt(params, bodies));
    CHECK(sim.phase() == SimPhase::AwaitingGround);
    CHECK(sim.epoch() == epoch);

    // Invalidating from the wait is a transition into Off: epoch bump.
    sim.simInvalidate("test");
    CHECK(sim.phase() == SimPhase::Off);
    CHECK(sim.epoch() == epoch + 1);
    sim.destroy();
}

TEST_CASE("water sim control: disabled or baseless stays off") {
    core::JobSystem jobs { 1 };
    WaterSimControl sim;
    sim.create(jobs);
    configureTiny(sim);
    const auto bodies = makeBodies(TerrainParams {});

    SUBCASE("disabled") {
        const TerrainParams params = makeStoryParams();
        sim.simConfig().enabled = false;
        sim.update(frameAt(params, bodies));
        CHECK(sim.phase() == SimPhase::Off);
        CHECK(!sim.simIsPreRolling());
        // Re-enabling enters on the next frame.
        sim.simConfig().enabled = true;
        sim.update(frameAt(params, bodies));
        CHECK(sim.phase() == SimPhase::PreRolling);
    }
    SUBCASE("no terrain base") {
        const TerrainParams params; // base null
        sim.update(frameAt(params, bodies));
        CHECK(sim.phase() == SimPhase::Off);
    }
    sim.destroy();
}

TEST_CASE("water sim control: a ground change asks the renderer to clear "
          "the frozen trail") {
    core::JobSystem jobs { 1 };
    WaterSimControl sim;
    sim.create(jobs);
    configureTiny(sim);
    sim.simConfig().enabled = false; // no job: the request alone
    const TerrainParams params = makeStoryParams();
    const auto bodies = makeBodies(params);

    sim.notifySimGroundChanged();
    const auto& requests = sim.update(frameAt(params, bodies));
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::ClearFrozenMeshes);
    // Consumed: the next frame carries nothing.
    CHECK(sim.update(frameAt(params, bodies)).empty());
    sim.destroy();
}

TEST_CASE("water sim control: destroy keeps the config, the epoch moves") {
    core::JobSystem jobs { 1 };
    WaterSimControl sim;
    sim.create(jobs);
    configureTiny(sim);
    sim.simConfig().timeScale = 3.5f;
    const TerrainParams params = makeStoryParams();
    const auto bodies = makeBodies(params);
    sim.update(frameAt(params, bodies));
    REQUIRE(sim.phase() == SimPhase::PreRolling);
    const u32 epoch = sim.epoch();

    sim.destroy();
    CHECK(sim.phase() == SimPhase::Off);
    CHECK(sim.epoch() == epoch + 1); // the in-flight solve is orphaned
    CHECK(sim.simConfig().timeScale == 3.5f);
    CHECK(sim.simConfig().span == 32.0f);
    CHECK(!sim.simIsValid());
    CHECK(sim.simWetCellCount() == 0);
}
