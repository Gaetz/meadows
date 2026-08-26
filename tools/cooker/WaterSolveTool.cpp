#include "WaterSolveTool.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <glm/glm.hpp>
#include <stb_image_write.h>

#include "engine/core/Clock.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/TileBake.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"

namespace cooker {

int waterSolve(char** argv, int argc) {
    using namespace render::terraingen;
    const u32 seed = static_cast<u32>(std::strtoul(argv[2], nullptr, 10));
    const i32 tx = std::atoi(argv[3]);
    const i32 tz = std::atoi(argv[4]);
    const char* outPath = argv[5];
    f32 texel = 8.0f;
    if (argc >= 7) {
        texel = static_cast<f32>(std::atof(argv[6]));
    }
    WaterSolveParams solve;
    if (argc >= 8) {
        solve.rainRate = static_cast<f32>(std::atof(argv[7]));
    }

    TileBakeParams params;
    params.worldSeed = seed;
    solve.seaLevel = params.macro.seaLevel;
    LOG_INFO("water-solve: baking tile ({}, {})...", tx, tz);
    core::TimePoint start = core::clockNow();
    const TileBakeResult baked = bakeTile(params, tx, tz);
    LOG_INFO("water-solve: bake {:.1f} s", core::secondsSince(start));
    const render::TerrainRegion& region = baked.region;

    // Solver grid: the region extent resampled at `texel`, through the
    // SAME query path every diagnostic uses (terrain::height on a
    // TerrainBase — the proven decode of the region layout).
    auto base = std::make_shared<render::TerrainBase>();
    base->regions.push_back(region);
    render::TerrainParams tp;
    tp.base = base;
    GridSpec spec;
    spec.originX = region.originX;
    spec.originZ = region.originZ;
    spec.texelSize = texel;
    spec.n = static_cast<u32>(region.spanX() / texel) + 1;
    vector<f32> ground(spec.cells());
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            ground[static_cast<size_t>(row) * spec.n + col] =
                render::terrain::height(tp, spec.x(col), spec.z(row));
        }
    }

    // Boundary inflow: master courses ENTERING the window inject their
    // whole upstream discharge (true area x rain — the same water the
    // solver would have gathered had the grid extended upstream).
    vector<WaterSource> sources;
    {
        ProceduralControlParams cp = params.controls;
        cp.seed = seed;
        const ProceduralControls controls { cp };
        MasterNetworkParams net = params.network;
        net.seaLevel = params.macro.seaLevel;
        const f32 maxX =
            spec.originX + static_cast<f32>(spec.n - 1) * texel;
        const f32 maxZ =
            spec.originZ + static_cast<f32>(spec.n - 1) * texel;
        const auto master = masterRiversNear(
            controls, params.macro, net, spec.originX - 4000.0f,
            spec.originZ - 4000.0f, maxX + 4000.0f, maxZ + 4000.0f);
        const auto inside = [&](const MasterNode& node) {
            return node.x >= spec.originX && node.x <= maxX &&
                   node.z >= spec.originZ && node.z <= maxZ;
        };
        for (const auto& river : master) {
            for (size_t k = 1; k < river.nodes.size(); ++k) {
                if (!inside(river.nodes[k - 1]) &&
                    inside(river.nodes[k])) {
                    // Linear rain x true area reaches Rhône-grade
                    // torrents (5400 m3/s measured) — cap at a big-
                    // fleuve discharge until a sub-linear runoff law
                    // replaces it.
                    sources.push_back(
                        { river.nodes[k].x, river.nodes[k].z,
                          glm::min(river.nodes[k].area *
                                       solve.rainRate,
                                   350.0f) });
                }
            }
        }
        f32 total = 0.0f;
        for (const WaterSource& s : sources) {
            total += s.discharge;
        }
        LOG_INFO("water-solve: {} boundary sources, {:.1f} m3/s total",
                 sources.size(), total);
    }

    LOG_INFO("water-solve: solving {}x{} at {} m/cell (rain {})...",
             spec.n, spec.n, texel, solve.rainRate);
    start = core::clockNow();
    const WaterSolveResult water =
        solveSteadyWater(spec, ground, solve, &sources);
    const f64 solveSeconds = core::secondsSince(start);

    // Stats: wetted land fraction, depth percentiles over wet land.
    u32 land = 0;
    u32 wet = 0;
    f32 maxDepth = 0.0f;
    f32 maxSpeed = 0.0f;
    for (size_t i = 0; i < spec.cells(); ++i) {
        if (ground[i] < solve.seaLevel) {
            continue;
        }
        ++land;
        if (water.depth[i] > 0.0f) {
            ++wet;
            maxDepth = glm::max(maxDepth, water.depth[i]);
            maxSpeed = glm::max(
                maxSpeed, std::hypot(water.velocityX[i],
                                     water.velocityZ[i]));
        }
    }
    LOG_INFO("water-solve: {} iterations in {:.1f} s (residual {}), "
             "wet land {:.2f}%, max depth {:.2f} m, max speed {:.2f} "
             "m/s",
             water.iterations, solveSeconds, water.residual,
             land ? 100.0f * static_cast<f32>(wet) /
                        static_cast<f32>(land)
                  : 0.0f,
             maxDepth, maxSpeed);

    // Judgment map: hillshade greys, water blues by depth, rapids
    // whitened by speed.
    const int n = static_cast<int>(spec.n);
    vector<u8> pixels(static_cast<size_t>(n) * n * 3);
    const auto groundAt = [&](i32 c, i32 r) {
        c = glm::clamp(c, 0, n - 1);
        r = glm::clamp(r, 0, n - 1);
        return ground[static_cast<size_t>(r) * spec.n +
                      static_cast<size_t>(c)];
    };
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n +
                             static_cast<size_t>(col);
            const f32 gx = groundAt(col - 1, row) - groundAt(col + 1, row);
            const f32 gz = groundAt(col, row - 1) - groundAt(col, row + 1);
            const Vec3 normal = glm::normalize(
                Vec3 { gx, 2.0f * spec.texelSize, gz });
            const f32 light = glm::clamp(
                0.35f + 0.65f * glm::dot(normal,
                                         glm::normalize(Vec3 {
                                             -0.45f, 0.8f, -0.4f })),
                0.0f, 1.0f);
            f32 r = light;
            f32 g = light;
            f32 b = light;
            const f32 d = water.depth[i];
            const bool sea = ground[i] < solve.seaLevel;
            if (d > 0.0f || sea) {
                const f32 t = glm::clamp(
                    std::sqrt((sea ? solve.seaLevel - ground[i] : d) /
                              12.0f),
                    0.0f, 1.0f);
                r = glm::mix(0.30f, 0.05f, t);
                g = glm::mix(0.55f, 0.18f, t);
                b = glm::mix(0.70f, 0.45f, t);
                const f32 speed = std::hypot(water.velocityX[i],
                                             water.velocityZ[i]);
                const f32 foam =
                    glm::clamp((speed - 1.0f) * 0.4f, 0.0f, 0.5f);
                r += foam;
                g += foam;
                b += foam;
            }
            const size_t at = (static_cast<size_t>(row) * n + col) * 3;
            pixels[at + 0] =
                static_cast<u8>(glm::clamp(r, 0.0f, 1.0f) * 255.0f);
            pixels[at + 1] =
                static_cast<u8>(glm::clamp(g, 0.0f, 1.0f) * 255.0f);
            pixels[at + 2] =
                static_cast<u8>(glm::clamp(b, 0.0f, 1.0f) * 255.0f);
        }
    }
    if (!stbi_write_png(outPath, n, n, 3, pixels.data(), n * 3)) {
        LOG_ERROR("water-solve: failed to write {}", outPath);
        return 1;
    }
    LOG_INFO("water-solve: wrote {} ({}x{}, {} m/px)", outPath, n, n,
             texel);
    return 0;
}

} // namespace cooker
