#include "game/scenes/SpiritDirector.hpp"

#include <cmath>

#include "data/forms/FormQuery.hpp"
#include "data/forms/SpiritForms.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/terrain/WaterQuery.hpp"

namespace game {

void SpiritDirector::build(const data::FormDatabase& forms) {
    table = world::compileSpiritRules(forms);
    for (const str& error : table.errors) {
        LOG_WARN("Spirits: {}", error);
    }
    groundProps = world::groundPropsFrom(table);
    jetFx.fill(core::Guid {});
    holdFx.fill(core::Guid {});
    fieldFx.fill(core::Guid {});
    data::forEach<data::SpiritForm>(forms, [&](const data::SpiritForm& spirit) {
        const auto kind = render::terrain::spiritFromName(spirit.name);
        if (kind != render::terrain::SpiritKind::kCount) {
            jetFx[static_cast<size_t>(kind)] = spirit.jetParticles;
            holdFx[static_cast<size_t>(kind)] = spirit.holdParticles;
            fieldFx[static_cast<size_t>(kind)] = spirit.fieldParticles;
        }
        if (kind == render::terrain::SpiritKind::Earth) {
            lift.quadratic = spirit.liftQuadratic;
            lift.min = spirit.liftMin;
            lift.max = spirit.liftMax;
        }
        if (kind == render::terrain::SpiritKind::Fire) {
            fireParams.spreadRate = spirit.spreadRate;
            fireParams.ignitionPoints = glm::max(spirit.ignitionPoints, 0.01f);
            fireParams.spreadBudgetPerTick =
                static_cast<u32>(glm::max(spirit.spreadBudgetPerTick, 1));
            if (spirit.decayPerSecond > 0.0f) {
                fireParams.heatDecay = spirit.decayPerSecond;
            }
        }
    });
    holdSource.reset();
    sources.apply(forms);
    if (!sources.entries().empty()) {
        LOG_INFO("Spirits: {} source(s) placed from the records",
                 sources.entries().size());
    }
}

core::Guid SpiritDirector::spawn(render::terrain::SpiritKind kind, f32 x,
                                 f32 z, f32 rate, f32 radius,
                                 f32 durationSimSeconds,
                                 const core::Guid& worldspace, f32 dirX,
                                 f32 dirZ) {
    render::terrain::SpiritSource source;
    source.kind = kind;
    source.x = x;
    source.z = z;
    source.dirX = dirX;
    source.dirZ = dirZ;
    source.rate = rate;
    source.radius = radius;
    source.remaining = durationSimSeconds;
    return sources.add(source, worldspace);
}

// --- The fire lane ---------------------------------------------------------

void SpiritDirector::ignite(f32 x, f32 z, f32 radius, f32 heat) {
    if (heat <= 0.0f) {
        return;
    }
    fireIgnitions.push_back({ x, z, glm::max(radius, 0.5f), heat });
}

void SpiritDirector::resetFire() {
    ++fireEpoch; // an in-flight job lands stale and is dropped
    fireGrid.reset();
    fireIgnitions.clear();
    fireActive = false;
    fireAccum = 0.0f;
    fireMask.clear();
    fireMaskSpec = {};
    fireCenters.clear();
    lastFireStats = {};
}

bool SpiritDirector::updateFire(core::JobSystem& jobs, const FireFrame& frame,
                                f32 simSeconds) {
    using render::terrain::FireGrid;
    bool landed = false;
    world::FireJobOutput out;
    while (fireShared->done.tryPop(out)) {
        fireInFlight = false;
        if (out.epoch != fireEpoch) {
            continue; // the map changed under it
        }
        const bool wasActive = fireActive;
        fireGrid = std::make_unique<FireGrid>(std::move(out.grid));
        fireMask = std::move(out.scorch);
        fireMaskSpec = fireGrid->spec;
        fireCenters = std::move(out.burning);
        lastFireStats = out.stats;
        fireMs = out.millis;
        fireActive = out.active;
        landed = true;
        if (wasActive && !fireActive) {
            LOG_INFO("Fire: out — {} cell(s) burnt", lastFireStats.burnt);
        }
    }
    if (fireActive) {
        fireAccum += glm::max(simSeconds, 0.0f);
    }
    if (fireInFlight || !frame.params) {
        return landed;
    }
    const bool sparks = !fireIgnitions.empty();
    if (!sparks && !fireActive) {
        return landed; // the lane idles: no job while nothing burns
    }
    u32 steps = static_cast<u32>(std::floor(fireAccum / fireParams.dt));
    steps = glm::min(steps, kMaxStepsPerJob);
    if (steps == 0) {
        if (!sparks) {
            return landed; // not due yet
        }
        steps = 1; // a spark lands now, whatever the clock says
    }
    fireAccum = glm::max(0.0f, fireAccum - static_cast<f32>(steps) * fireParams.dt);

    world::FireJobInput in;
    if (fireGrid) {
        in.grid = std::move(*fireGrid);
        fireGrid.reset();
        i32 dCol = 0, dRow = 0;
        world::FireWindow::scrollFor(in.grid.spec, frame.focus.x, frame.focus.y,
                                     &dCol, &dRow);
        in.spec = (dCol != 0 || dRow != 0)
                      ? world::FireWindow::specFor(frame.focus.x, frame.focus.y)
                      : in.grid.spec;
    } else {
        in.spec = world::FireWindow::specFor(frame.focus.x, frame.focus.y);
    }
    in.params = fireParams;
    in.ignitions = std::move(fireIgnitions);
    fireIgnitions.clear();
    in.steps = steps;
    in.epoch = fireEpoch;
    // Value copies for the worker: the terrain (one shared copy for both
    // samplers), the material props, the water snapshot and bodies.
    auto params = std::make_shared<const render::TerrainParams>(*frame.params);
    const world::GroundProps props = groundProps;
    in.fuel = [params, props](f32 x, f32 z) {
        const f32 h = render::terrain::height(*params, x, z);
        const Vec3 n = render::terrain::normal(*params, x, z);
        const render::terrain::RegionFields fields =
            render::terrain::regionFieldsAt(*params, x, z);
        const render::terrain::MaterialWeights w =
            render::terrain::materialWeightsAt(*params, x, z, h, n, fields);
        return world::fuelFromWeights(
            props, { w.grass, w.rock, w.cliff, w.snow, w.sand }, fields.wetness);
    };
    const sptr<const render::terrain::WaterSimSnapshot> water = frame.water;
    const sptr<const render::WaterBodies> bodies = frame.bodies;
    const f32 seaLevel = frame.seaLevel;
    in.wet = [params, water, bodies, seaLevel](f32 x, f32 z) {
        const render::terrain::WaterQuery q { water.get(), bodies.get(),
                                              seaLevel };
        const f32 ground = render::terrain::height(*params, x, z);
        const std::optional<f32> surface =
            render::terrain::waterSurfaceQuery(q, x, z, ground);
        return surface && *surface - ground > 0.03f;
    };
    fireInFlight = true;
    jobs.enqueue([shared = fireShared, input = std::move(in)]() mutable {
        shared->done.push(world::runFireJob(std::move(input)));
    });
    return landed;
}

} // namespace game
