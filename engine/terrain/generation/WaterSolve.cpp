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
namespace {

// One relaxation run on a single grid level (`initialDepth` empty =
// cold/flood start per params.warmStart).
WaterSolveResult solveLevel(const GridSpec& spec,
                            const vector<f32>& terrain,
                            const WaterSolveParams& params,
                            const vector<WaterSource>* sources,
                            const vector<f32>* initialDepth);

} // namespace

WaterSolveResult solveSteadyWater(const GridSpec& spec,
                                  const vector<f32>& terrain,
                                  const WaterSolveParams& params,
                                  const vector<WaterSource>* sources) {
    if (!params.multigrid || spec.n <= params.multigridMinN ||
        (spec.n - 1) % 2 != 0) {
        return solveLevel(spec, terrain, params, sources, nullptr);
    }
    // Coarse level: half resolution, min-downsampled terrain (a carved
    // channel must survive the downsample or the coarse routing digs
    // elsewhere), full iteration budget (cheap), recursive.
    GridSpec coarse;
    coarse.originX = spec.originX;
    coarse.originZ = spec.originZ;
    coarse.texelSize = spec.texelSize * 2.0f;
    coarse.n = (spec.n - 1) / 2 + 1;
    vector<f32> coarseTerrain(coarse.cells());
    for (u32 row = 0; row < coarse.n; ++row) {
        for (u32 col = 0; col < coarse.n; ++col) {
            f32 low = 1.0e30f;
            for (u32 dz = 0; dz < 2; ++dz) {
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 fc = glm::min(col * 2 + dx, spec.n - 1);
                    const u32 fr = glm::min(row * 2 + dz, spec.n - 1);
                    low = glm::min(
                        low, terrain[static_cast<size_t>(fr) * spec.n +
                                     fc]);
                }
            }
            coarseTerrain[static_cast<size_t>(row) * coarse.n + col] =
                low;
        }
    }
    WaterSolveParams coarseParams = params;
    // Coarse levels only need the large-scale state — the fine level
    // corrects locally; a full budget down there was half the wall.
    coarseParams.maxIterations =
        glm::min(params.maxIterations, 4000u);
    const WaterSolveResult below =
        solveSteadyWater(coarse, coarseTerrain, coarseParams, sources);
    // Transfer the SURFACE (not the depth): the fine terrain differs
    // from the min-downsampled one, and a depth copied onto a higher
    // fine cell would invent water on ridges.
    vector<f32> warm(spec.cells(), 0.0f);
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            const f32 fx = static_cast<f32>(col) * 0.5f;
            const f32 fz = static_cast<f32>(row) * 0.5f;
            const u32 c0 = glm::min(static_cast<u32>(fx), coarse.n - 2);
            const u32 r0 = glm::min(static_cast<u32>(fz), coarse.n - 2);
            const f32 tc = fx - static_cast<f32>(c0);
            const f32 tr = fz - static_cast<f32>(r0);
            const auto surfaceAt = [&](u32 c, u32 r) {
                const size_t i = static_cast<size_t>(r) * coarse.n + c;
                return coarseTerrain[i] + below.depth[i];
            };
            const f32 surface = glm::mix(
                glm::mix(surfaceAt(c0, r0), surfaceAt(c0 + 1, r0), tc),
                glm::mix(surfaceAt(c0, r0 + 1),
                         surfaceAt(c0 + 1, r0 + 1), tc),
                tr);
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            warm[i] = glm::max(0.0f, surface - terrain[i]);
        }
    }
    WaterSolveParams fine = params;
    fine.maxIterations = params.fineIterations;
    return solveLevel(spec, terrain, fine, sources, &warm);
}

namespace {

WaterSolveResult solveLevel(const GridSpec& spec,
                            const vector<f32>& terrain,
                            const WaterSolveParams& params,
                            const vector<WaterSource>* sources,
                            const vector<f32>* initialDepth) {
    const i32 n = static_cast<i32>(spec.n);
    const size_t cells = spec.cells();
    WaterSolveResult out;
    out.spec = spec;
    out.depth.assign(cells, 0.0f);
    out.velocityX.assign(cells, 0.0f);
    out.velocityZ.assign(cells, 0.0f);
    out.flux.assign(cells, 0.0f);

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
    if (initialDepth) {
        depth = *initialDepth;
    } else if (params.warmStart) {
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
    // Hydraulic head buffer + hoisted constants: the lambda-per-pipe
    // version with bounds checks ran ~50 ns/cell and dominated the
    // whole solve.
    vector<f32> headBuf(cells);
    const f32 friction = params.friction;
    const f32 eqCap = cellArea / (2.0f * params.dt);
    const f32 dtOverArea = params.dt / cellArea;
    const f32 net = params.dt * (params.rainRate -
                                 params.evaporationRate);
    // Border pipes are walls once and forever.
    for (i32 col = 0; col < n; ++col) {
        fN[at(col, 0)] = 0.0f;
        fS[at(col, n - 1)] = 0.0f;
    }
    for (i32 row = 0; row < n; ++row) {
        fW[at(0, row)] = 0.0f;
        fE[at(n - 1, row)] = 0.0f;
    }
    for (; iter < params.maxIterations; ++iter) {
        for (size_t i = 0; i < cells; ++i) {
            headBuf[i] = terrain[i] + depth[i];
        }
        // Pipe update (reads heads, writes fluxes). Interior cells run
        // check-free; the one-texel border keeps its walls (zeroed
        // above, only its inward pipes update).
        // Equalization clamp: one step may never push a pair past
        // level — kills the ringing at cliffs (173 m/s ghosts,
        // measured) while /(2 dt) still lets gentle channels carry
        // their catchment's discharge.
        const auto updatePipes = [&](size_t i, bool e, bool w, bool s,
                                     bool nn) {
            const f32 head = headBuf[i];
            f32 total = 0.0f;
            if (e) {
                const f32 dh = head - headBuf[i + 1];
                fE[i] = glm::min(
                    glm::max(0.0f, fE[i] * friction + gain * dh),
                    glm::max(dh * eqCap, 0.0f));
                total += fE[i];
            }
            if (w) {
                const f32 dh = head - headBuf[i - 1];
                fW[i] = glm::min(
                    glm::max(0.0f, fW[i] * friction + gain * dh),
                    glm::max(dh * eqCap, 0.0f));
                total += fW[i];
            }
            if (s) {
                const f32 dh = head - headBuf[i + spec.n];
                fS[i] = glm::min(
                    glm::max(0.0f, fS[i] * friction + gain * dh),
                    glm::max(dh * eqCap, 0.0f));
                total += fS[i];
            }
            if (nn) {
                const f32 dh = head - headBuf[i - spec.n];
                fN[i] = glm::min(
                    glm::max(0.0f, fN[i] * friction + gain * dh),
                    glm::max(dh * eqCap, 0.0f));
                total += fN[i];
            }
            // Never ship more than the cell holds.
            const f32 held = depth[i] * cellArea;
            if (total * params.dt > held && total > 0.0f) {
                const f32 k = held / (total * params.dt);
                fE[i] *= k;
                fW[i] *= k;
                fS[i] *= k;
                fN[i] *= k;
            }
        };
        for (i32 row = 1; row < n - 1; ++row) {
            const size_t base = static_cast<size_t>(row) * spec.n;
            for (i32 col = 1; col < n - 1; ++col) {
                updatePipes(base + col, true, true, true, true);
            }
        }
        for (i32 col = 0; col < n; ++col) {
            updatePipes(at(col, 0), col + 1 < n, col > 0, n > 1,
                        false);
            updatePipes(at(col, n - 1), col + 1 < n, col > 0, false,
                        n > 1);
        }
        for (i32 row = 1; row < n - 1; ++row) {
            updatePipes(at(0, row), true, false, true, true);
            updatePipes(at(n - 1, row), false, true, true, true);
        }
        // Depth update (flux divergence + rain - evaporation).
        for (i32 row = 1; row < n - 1; ++row) {
            const size_t base = static_cast<size_t>(row) * spec.n;
            for (i32 col = 1; col < n - 1; ++col) {
                const size_t i = base + col;
                const f32 inflow = fE[i - 1] + fW[i + 1] +
                                   fS[i - spec.n] + fN[i + spec.n];
                const f32 outflow = fE[i] + fW[i] + fS[i] + fN[i];
                next[i] = glm::max(
                    0.0f, depth[i] +
                              (inflow - outflow) * dtOverArea + net);
            }
        }
        // Border cells: open boundary — a fraction of their depth
        // leaks off-grid, as if the terrain continued.
        const f32 borderKeep = 1.0f - params.borderDrain;
        for (i32 col = 0; col < n; ++col) {
            for (const i32 row : { 0, n - 1 }) {
                const size_t i = at(col, row);
                f32 inflow = 0.0f;
                if (col > 0) {
                    inflow += fE[i - 1];
                }
                if (col + 1 < n) {
                    inflow += fW[i + 1];
                }
                if (row > 0) {
                    inflow += fS[i - spec.n];
                }
                if (row + 1 < n) {
                    inflow += fN[i + spec.n];
                }
                const f32 outflow = fE[i] + fW[i] + fS[i] + fN[i];
                next[i] = glm::max(
                    0.0f,
                    (depth[i] + (inflow - outflow) * dtOverArea + net) *
                        borderKeep);
            }
        }
        for (i32 row = 1; row < n - 1; ++row) {
            for (const i32 col : { 0, n - 1 }) {
                const size_t i = at(col, row);
                f32 inflow = 0.0f;
                if (col > 0) {
                    inflow += fE[i - 1];
                }
                if (col + 1 < n) {
                    inflow += fW[i + 1];
                }
                inflow += fS[i - spec.n] + fN[i + spec.n];
                const f32 outflow = fE[i] + fW[i] + fS[i] + fN[i];
                next[i] = glm::max(
                    0.0f,
                    (depth[i] + (inflow - outflow) * dtOverArea + net) *
                        borderKeep);
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
    // film noise. The FLUX field records every cell's through-discharge
    // BEFORE the drying: a course stays a course where its water runs
    // thin.
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = at(col, row);
            // At steady state outflow ~= inflow: the total outflow IS
            // the cell's through-discharge.
            out.flux[i] = fE[i] + fW[i] + fS[i] + fN[i];
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

} // namespace

} // namespace render::terraingen
