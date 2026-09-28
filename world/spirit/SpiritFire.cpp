#include "world/spirit/SpiritFire.hpp"

#include <chrono>
#include <cmath>

#include <glm/glm.hpp>

namespace world {

render::terraingen::GridSpec FireWindow::specFor(f32 focusX, f32 focusZ) {
    render::terraingen::GridSpec spec;
    spec.texelSize = kTexel;
    spec.n = static_cast<u32>(kSpan / kTexel) + 1;
    spec.originX = std::floor((focusX - kSpan * 0.5f) / kTexel) * kTexel;
    spec.originZ = std::floor((focusZ - kSpan * 0.5f) / kTexel) * kTexel;
    return spec;
}

void FireWindow::scrollFor(const render::terraingen::GridSpec& spec,
                           f32 focusX, f32 focusZ, i32* dCol, i32* dRow) {
    *dCol = 0;
    *dRow = 0;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    const f32 centerX = spec.originX + span * 0.5f;
    const f32 centerZ = spec.originZ + span * 0.5f;
    if (std::abs(focusX - centerX) <= kRecenter &&
        std::abs(focusZ - centerZ) <= kRecenter) {
        return;
    }
    const render::terraingen::GridSpec target = specFor(focusX, focusZ);
    *dCol = static_cast<i32>(
        std::lround((target.originX - spec.originX) / spec.texelSize));
    *dRow = static_cast<i32>(
        std::lround((target.originZ - spec.originZ) / spec.texelSize));
}

GroundProps groundPropsFrom(const SpiritRuleTable& rules) {
    static const char* const kNames[kGroundClasses] = { "grass", "rock", "cliff",
                                                        "snow", "sand" };
    GroundProps out {};
    for (size_t i = 0; i < kGroundClasses; ++i) {
        const i32 index = rules.classIndex(kNames[i]);
        if (index >= 0 && static_cast<size_t>(index) < rules.props.size()) {
            out[i] = rules.props[static_cast<size_t>(index)];
        }
    }
    return out;
}

render::terrain::FireCellFuel fuelFromWeights(const GroundProps& props,
                                              const GroundWeights& weights,
                                              f32 wetness) {
    f32 total = 0.0f;
    for (const f32 w : weights) {
        total += glm::max(w, 0.0f);
    }
    render::terrain::FireCellFuel out;
    if (total <= 1e-6f) {
        return out; // no ground: nothing burns
    }
    for (size_t i = 0; i < kGroundClasses; ++i) {
        const f32 w = glm::max(weights[i], 0.0f) / total;
        out.fuel += w * props[i].fuel;
        out.flammability += w * props[i].flammability;
        out.moisture += w * props[i].moisture;
    }
    out.moisture = glm::clamp(glm::max(out.moisture, wetness), 0.0f, 1.0f);
    return out;
}

FireJobOutput runFireJob(FireJobInput&& in) {
    using namespace render::terrain;
    const auto start = std::chrono::steady_clock::now();
    FireJobOutput out;
    out.epoch = in.epoch;
    FireGrid& grid = in.grid;
    if (!grid.valid()) {
        fireInitWindow(grid, in.spec);
    } else if (grid.spec.originX != in.spec.originX ||
               grid.spec.originZ != in.spec.originZ) {
        const i32 dCol = static_cast<i32>(std::lround(
            (in.spec.originX - grid.spec.originX) / grid.spec.texelSize));
        const i32 dRow = static_cast<i32>(std::lround(
            (in.spec.originZ - grid.spec.originZ) / grid.spec.texelSize));
        fireScrollWindow(grid, dCol, dRow);
    }
    for (const FireIgnition& spark : in.ignitions) {
        fireIgnite(grid, spark.x, spark.z, spark.radius, spark.heat, in.fuel);
    }
    FireStats stats;
    for (u32 i = 0; i < glm::max(in.steps, 1u); ++i) {
        fireStep(grid, in.params, in.fuel, in.wet, &stats);
    }
    out.stats = stats;
    fireScorch(grid, in.params, out.scorch);
    fireGlow(grid, in.params, out.glow);
    out.burning = fireBurningCenters(grid, in.maxCenters, in.params,
                                     in.params.emberSeconds);
    out.active = stats.burning > 0;
    out.grid = std::move(grid);
    out.millis = std::chrono::duration<f32, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count();
    return out;
}

} // namespace world
