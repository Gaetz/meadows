#include "engine/terrain/generation/WaterSolve.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/generation/FluvialErosion.hpp"

namespace render::terraingen {

// Virtual pipes (Mei et al. 2007 family): per cell, four outflow pipes
// accelerated by the hydraulic head difference to each neighbour; the
// outflow is rescaled so a cell never ships more water than it holds
// (which is what keeps the explicit scheme from exploding), then depths
// integrate the flux divergence. Friction damps the pipe momentum so
// the steady state settles instead of ringing.
WaterSolveResult solveSteadyWater(const GridSpec& spec,
                                  const vector<f32>& terrain,
                                  const WaterSolveParams& params,
                                  const vector<WaterSource>* sources) {
    const i32 n = static_cast<i32>(spec.n);
    const size_t cells = spec.cells();
    WaterSolveResult out;
    out.spec = spec;
    out.depth.assign(cells, 0.0f);
    out.velocityX.assign(cells, 0.0f);
    out.velocityZ.assign(cells, 0.0f);

    // Pipes: +x, -x, +z, -z outflow per cell.
    vector<f32> fE(cells, 0.0f);
    vector<f32> fW(cells, 0.0f);
    vector<f32> fS(cells, 0.0f);
    vector<f32> fN(cells, 0.0f);
    vector<f32> depth = out.depth;
    vector<f32> next(cells, 0.0f);

    const f32 texel = spec.texelSize;
    const f32 cellArea = texel * texel;
    // Pipe gain: g * dt * (pipe cross-section / length). The classic
    // formulation uses the texel as both — the constant only scales how
    // fast the system settles, the equilibrium is the same.
    const f32 gain = params.gravity * params.dt * texel;

    const auto at = [&](i32 col, i32 row) {
        return static_cast<size_t>(row) * spec.n +
               static_cast<size_t>(col);
    };
    // Sea cells: surface pinned to sea level (they absorb and supply).
    const auto pinSea = [&](vector<f32>& d) {
        for (size_t i = 0; i < cells; ++i) {
            if (terrain[i] < params.seaLevel) {
                d[i] = params.seaLevel - terrain[i];
            }
        }
    };
    if (params.warmStart) {
        const vector<f32> filled = priorityFloodFill(
            spec, terrain, params.seaLevel, 1.0e-4f);
        for (size_t i = 0; i < cells; ++i) {
            depth[i] = glm::max(0.0f, filled[i] - terrain[i]);
        }
    }
    pinSea(depth);

    f32 residual = 1.0e9f;
    u32 iter = 0;
    vector<f32> checkpoint = depth;
    for (; iter < params.maxIterations; ++iter) {
        // Pipe update (reads depth, writes fluxes).
        for (i32 row = 0; row < n; ++row) {
            for (i32 col = 0; col < n; ++col) {
                const size_t i = at(col, row);
                const f32 head = terrain[i] + depth[i];
                const auto pipe = [&](f32& f, i32 pc, i32 pr) {
                    if (pc < 0 || pr < 0 || pc >= n || pr >= n) {
                        f = 0.0f; // grid border: wall
                        return;
                    }
                    const size_t j = at(pc, pr);
                    const f32 dh = head - (terrain[j] + depth[j]);
                    f = glm::max(0.0f, f * params.friction + gain * dh);
                    // Equalization clamp: one step may never push the
                    // pair past level — this kills the ringing at
                    // cliffs (173 m/s ghosts, measured) while the /(2
                    // dt) bound still lets gentle-slope channels carry
                    // their catchment's discharge (the stricter /(8 dt)
                    // choked them and piled water instead).
                    f = glm::min(
                        f, glm::max(dh * cellArea /
                                        (2.0f * params.dt),
                                    0.0f));
                };
                pipe(fE[i], col + 1, row);
                pipe(fW[i], col - 1, row);
                pipe(fS[i], col, row + 1);
                pipe(fN[i], col, row - 1);
                // Never ship more than the cell holds.
                const f32 total =
                    (fE[i] + fW[i] + fS[i] + fN[i]) * params.dt;
                if (total > depth[i] * cellArea && total > 0.0f) {
                    const f32 k = depth[i] * cellArea / total;
                    fE[i] *= k;
                    fW[i] *= k;
                    fS[i] *= k;
                    fN[i] *= k;
                }
            }
        }
        // Depth update (flux divergence + rain).
        for (i32 row = 0; row < n; ++row) {
            for (i32 col = 0; col < n; ++col) {
                const size_t i = at(col, row);
                f32 inflow = 0.0f;
                if (col > 0) {
                    inflow += fE[at(col - 1, row)];
                }
                if (col + 1 < n) {
                    inflow += fW[at(col + 1, row)];
                }
                if (row > 0) {
                    inflow += fS[at(col, row - 1)];
                }
                if (row + 1 < n) {
                    inflow += fN[at(col, row + 1)];
                }
                const f32 outflow = fE[i] + fW[i] + fS[i] + fN[i];
                next[i] = glm::max(
                    0.0f, depth[i] +
                              params.dt * ((inflow - outflow) / cellArea +
                                           params.rainRate -
                                           params.evaporationRate));
            }
        }
        std::swap(depth, next);
        pinSea(depth);
        // Inject AFTER the sea pin: a source cell pinned to sea level
        // would swallow its discharge every iteration (measured — the
        // fleuve entry near a mouth sits below sea level).
        if (sources) {
            for (const WaterSource& source : *sources) {
                const i32 col = static_cast<i32>(std::lround(
                    (source.x - spec.originX) / texel));
                const i32 row = static_cast<i32>(std::lround(
                    (source.z - spec.originZ) / texel));
                if (col >= 0 && row >= 0 && col < n && row < n &&
                    terrain[at(col, row)] >= params.seaLevel) {
                    depth[at(col, row)] +=
                        source.discharge * params.dt / cellArea;
                }
            }
        }

        if ((iter + 1) % params.checkInterval == 0) {
            residual = 0.0f;
            for (size_t i = 0; i < cells; ++i) {
                residual = glm::max(residual,
                                    std::abs(depth[i] - checkpoint[i]));
            }
            checkpoint = depth;
            // Rain keeps adding water, so at equilibrium the depth
            // still creeps where it pools; the convergence bar
            // accounts for that.
            const f32 floor = params.rainRate * params.dt *
                              static_cast<f32>(params.checkInterval) *
                              2.0f;
            if (residual < glm::max(params.convergenceEps, floor)) {
                ++iter;
                break;
            }
        }
    }

    // Depth-averaged velocity from the net pipe flux, then dry the
    // film noise.
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = at(col, row);
            if (depth[i] <= params.dryThreshold) {
                depth[i] = 0.0f;
                continue;
            }
            const f32 flowX = fE[i] - fW[i];
            const f32 flowZ = fS[i] - fN[i];
            const f32 div = glm::max(depth[i], 0.05f) * texel;
            f32 vx = flowX / div;
            f32 vz = flowZ / div;
            // Thin-film ghosts at waterfalls divide by near-nothing —
            // cap at a physical torrent speed.
            const f32 speed = std::hypot(vx, vz);
            if (speed > 12.0f) {
                vx *= 12.0f / speed;
                vz *= 12.0f / speed;
            }
            out.velocityX[i] = vx;
            out.velocityZ[i] = vz;
        }
    }
    out.depth = std::move(depth);
    out.iterations = iter;
    out.residual = residual;
    return out;
}

} // namespace render::terraingen
