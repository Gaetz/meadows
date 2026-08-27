#include "engine/terrain/WaterSim.hpp"

#include <cmath>
#include <cstring>

#include <glm/glm.hpp>

#include "engine/terrain/generation/FluvialErosion.hpp"

namespace render::terrain {

namespace {

using terraingen::GridSpec;
using terraingen::WaterSource;

void pinSea(const WaterSimState& state, vector<f32>& depth,
            f32 seaLevel) {
    const size_t cells = state.spec.cells();
    for (size_t i = 0; i < cells; ++i) {
        if (state.terrain[i] < seaLevel) {
            depth[i] = seaLevel - state.terrain[i];
        }
    }
}

void zeroBorderWalls(WaterSimState& state) {
    const i32 n = static_cast<i32>(state.spec.n);
    const auto at = [&](i32 col, i32 row) {
        return static_cast<size_t>(row) * state.spec.n +
               static_cast<size_t>(col);
    };
    for (i32 col = 0; col < n; ++col) {
        state.fN[at(col, 0)] = 0.0f;
        state.fS[at(col, n - 1)] = 0.0f;
    }
    for (i32 row = 0; row < n; ++row) {
        state.fW[at(0, row)] = 0.0f;
        state.fE[at(n - 1, row)] = 0.0f;
    }
}

void fillTerrain(const GridSpec& spec, const HeightFn& height,
                 vector<f32>& terrain) {
    terrain.resize(spec.cells());
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            terrain[static_cast<size_t>(row) * spec.n + col] =
                height(spec.x(col), spec.z(row));
        }
    }
}

} // namespace

void initWindow(WaterSimState& state, const GridSpec& spec,
                const HeightFn& height, f32 seaLevel) {
    state.spec = spec;
    const size_t cells = spec.cells();
    fillTerrain(spec, height, state.terrain);
    state.depth.assign(cells, 0.0f);
    state.fE.assign(cells, 0.0f);
    state.fW.assign(cells, 0.0f);
    state.fS.assign(cells, 0.0f);
    state.fN.assign(cells, 0.0f);
    state.headBuf.assign(cells, 0.0f);
    state.scratch.assign(cells, 0.0f);
    const vector<f32> filled = terraingen::priorityFloodFill(
        spec, state.terrain, seaLevel, 1.0e-4f);
    for (size_t i = 0; i < cells; ++i) {
        state.depth[i] = glm::max(0.0f, filled[i] - state.terrain[i]);
    }
    pinSea(state, state.depth, seaLevel);
    zeroBorderWalls(state);
}

void stepWindow(WaterSimState& state, const WaterSimParams& params,
                const vector<WaterSource>& sources, u32 substeps) {
    // The offline solveLevel kernel verbatim (pipes -> mass limiter ->
    // divergence -> open borders -> sea pin -> sources), minus
    // convergence checking — the WaterSimTest equilibrium cross-check
    // guards the two copies against silent divergence.
    const GridSpec& spec = state.spec;
    const i32 n = static_cast<i32>(spec.n);
    const size_t cells = spec.cells();
    const f32 texel = spec.texelSize;
    const f32 cellArea = texel * texel;
    const f32 gain = params.gravity * params.dt * texel;
    const f32 friction = params.friction;
    const f32 eqCap = cellArea / (2.0f * params.dt);
    const f32 dtOverArea = params.dt / cellArea;
    const f32 net =
        params.dt * (params.rainRate - params.evaporationRate);
    // Per-second border drain -> per-substep keep factor.
    const f32 borderKeep = std::pow(
        glm::clamp(1.0f - params.borderDrainPerSecond, 0.0f, 1.0f),
        params.dt);

    vector<f32>& depth = state.depth;
    vector<f32>& fE = state.fE;
    vector<f32>& fW = state.fW;
    vector<f32>& fS = state.fS;
    vector<f32>& fN = state.fN;
    vector<f32>& headBuf = state.headBuf;
    vector<f32>& next = state.scratch;
    const vector<f32>& terrain = state.terrain;
    const auto at = [&](i32 col, i32 row) {
        return static_cast<size_t>(row) * spec.n +
               static_cast<size_t>(col);
    };

    for (u32 step = 0; step < substeps; ++step) {
        for (size_t i = 0; i < cells; ++i) {
            headBuf[i] = terrain[i] + depth[i];
        }
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
            updatePipes(at(col, 0), col + 1 < n, col > 0, n > 1, false);
            updatePipes(at(col, n - 1), col + 1 < n, col > 0, false,
                        n > 1);
        }
        for (i32 row = 1; row < n - 1; ++row) {
            updatePipes(at(0, row), true, false, true, true);
            updatePipes(at(n - 1, row), false, true, true, true);
        }
        for (i32 row = 1; row < n - 1; ++row) {
            const size_t base = static_cast<size_t>(row) * spec.n;
            for (i32 col = 1; col < n - 1; ++col) {
                const size_t i = base + col;
                const f32 inflow = fE[i - 1] + fW[i + 1] +
                                   fS[i - spec.n] + fN[i + spec.n];
                const f32 outflow = fE[i] + fW[i] + fS[i] + fN[i];
                next[i] = glm::max(
                    0.0f,
                    depth[i] + (inflow - outflow) * dtOverArea + net);
            }
        }
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
        pinSea(state, depth, params.seaLevel);
        // After the pin (a pinned source cell swallows its discharge).
        for (const WaterSource& source : sources) {
            const i32 col = static_cast<i32>(
                std::lround((source.x - spec.originX) / texel));
            const i32 row = static_cast<i32>(
                std::lround((source.z - spec.originZ) / texel));
            if (col >= 0 && row >= 0 && col < n && row < n &&
                terrain[at(col, row)] >= params.seaLevel) {
                depth[at(col, row)] +=
                    source.discharge * params.dt / cellArea;
            }
        }
    }
}

void scrollWindow(WaterSimState& state, i32 dCol, i32 dRow,
                  const HeightFn& height, f32 seaLevel) {
    const i32 n = static_cast<i32>(state.spec.n);
    if ((dCol == 0 && dRow == 0) || std::abs(dCol) >= n ||
        std::abs(dRow) >= n) {
        if (dCol != 0 || dRow != 0) {
            // Jumped a whole window: teleport-grade re-init.
            GridSpec spec = state.spec;
            spec.originX += static_cast<f32>(dCol) * spec.texelSize;
            spec.originZ += static_cast<f32>(dRow) * spec.texelSize;
            initWindow(state, spec, height, seaLevel);
        }
        return;
    }
    vector<f32>* planes[] = { &state.terrain, &state.depth, &state.fE,
                              &state.fW,      &state.fS,    &state.fN };
    const auto at = [&](i32 col, i32 row) {
        return static_cast<size_t>(row) * state.spec.n +
               static_cast<size_t>(col);
    };

    // --- X shift, per row (avoids row-wrap bleed).
    if (dCol != 0) {
        const i32 keep = n - std::abs(dCol);
        for (vector<f32>* plane : planes) {
            f32* data = plane->data();
            for (i32 row = 0; row < n; ++row) {
                f32* r = data + at(0, row);
                if (dCol > 0) {
                    std::memmove(r, r + dCol,
                                 static_cast<size_t>(keep) *
                                     sizeof(f32));
                } else {
                    std::memmove(r - dCol, r,
                                 static_cast<size_t>(keep) *
                                     sizeof(f32));
                }
            }
        }
        state.spec.originX +=
            static_cast<f32>(dCol) * state.spec.texelSize;
        // Entered columns: terrain, then depth swept outward from the
        // surviving edge surface (lakes arrive full), pipes copied
        // from the surviving edge (rivers arrive moving).
        const i32 firstNew = dCol > 0 ? keep : 0;
        const i32 lastNew = dCol > 0 ? n - 1 : std::abs(dCol) - 1;
        const i32 sweepFrom = dCol > 0 ? firstNew : lastNew;
        const i32 sweepStep = dCol > 0 ? 1 : -1;
        for (i32 row = 0; row < n; ++row) {
            for (i32 col = firstNew; col <= lastNew; ++col) {
                state.terrain[at(col, row)] = height(
                    state.spec.x(static_cast<u32>(col)),
                    state.spec.z(static_cast<u32>(row)));
            }
            const i32 edge = sweepFrom - sweepStep; // survivor column
            for (i32 col = sweepFrom; col >= firstNew && col <= lastNew;
                 col += sweepStep) {
                const size_t i = at(col, row);
                const size_t prev = at(col - sweepStep, row);
                const f32 prevSurface =
                    state.terrain[prev] + state.depth[prev];
                state.depth[i] = glm::max(
                    0.0f, prevSurface - state.terrain[i]);
                state.fE[i] = state.fE[at(edge, row)];
                state.fW[i] = state.fW[at(edge, row)];
                state.fS[i] = state.fS[at(edge, row)];
                state.fN[i] = state.fN[at(edge, row)];
            }
        }
    }

    // --- Z shift, whole-block move.
    if (dRow != 0) {
        const i32 keep = n - std::abs(dRow);
        const size_t rowBytes = static_cast<size_t>(n) * sizeof(f32);
        for (vector<f32>* plane : planes) {
            f32* data = plane->data();
            if (dRow > 0) {
                std::memmove(data, data + at(0, dRow),
                             static_cast<size_t>(keep) * rowBytes);
            } else {
                std::memmove(data + at(0, -dRow), data,
                             static_cast<size_t>(keep) * rowBytes);
            }
        }
        state.spec.originZ +=
            static_cast<f32>(dRow) * state.spec.texelSize;
        const i32 firstNew = dRow > 0 ? keep : 0;
        const i32 lastNew = dRow > 0 ? n - 1 : std::abs(dRow) - 1;
        const i32 sweepFrom = dRow > 0 ? firstNew : lastNew;
        const i32 sweepStep = dRow > 0 ? 1 : -1;
        const i32 edge = sweepFrom - sweepStep;
        for (i32 row = firstNew; row <= lastNew; ++row) {
            for (i32 col = 0; col < n; ++col) {
                state.terrain[at(col, row)] = height(
                    state.spec.x(static_cast<u32>(col)),
                    state.spec.z(static_cast<u32>(row)));
            }
        }
        for (i32 row = sweepFrom; row >= firstNew && row <= lastNew;
             row += sweepStep) {
            for (i32 col = 0; col < n; ++col) {
                const size_t i = at(col, row);
                const size_t prev = at(col, row - sweepStep);
                const f32 prevSurface =
                    state.terrain[prev] + state.depth[prev];
                state.depth[i] = glm::max(
                    0.0f, prevSurface - state.terrain[i]);
                state.fE[i] = state.fE[at(col, edge)];
                state.fW[i] = state.fW[at(col, edge)];
                state.fS[i] = state.fS[at(col, edge)];
                state.fN[i] = state.fN[at(col, edge)];
            }
        }
    }

    pinSea(state, state.depth, seaLevel);
    zeroBorderWalls(state);
}

void refreshTerrain(WaterSimState& state, const HeightFn& height) {
    fillTerrain(state.spec, height, state.terrain);
}

void extractSnapshot(const WaterSimState& state,
                     const WaterSimParams& params,
                     WaterSimSnapshot& out) {
    const GridSpec& spec = state.spec;
    const size_t cells = spec.cells();
    out.spec = spec;
    out.marginCells = params.marginCells;
    out.surface.assign(cells, kWaterInfoDry);
    out.depth.assign(cells, 0.0f);
    out.velX.assign(cells, 0.0f);
    out.velZ.assign(cells, 0.0f);
    out.display.resize(cells);
    const f32 texel = spec.texelSize;
    for (size_t i = 0; i < cells; ++i) {
        const f32 d = state.depth[i];
        out.display[i] = state.terrain[i] - 0.25f; // dry tuck default
        if (d <= params.dryThreshold) {
            continue;
        }
        // Sea-pinned: the ocean sheet's territory, read dry.
        if (state.terrain[i] < params.seaLevel &&
            state.terrain[i] + d <= params.seaLevel + 0.01f) {
            continue;
        }
        out.depth[i] = d;
        out.surface[i] = state.terrain[i] + d;
        out.display[i] = out.surface[i];
        const f32 flowX = state.fE[i] - state.fW[i];
        const f32 flowZ = state.fS[i] - state.fN[i];
        const f32 div = glm::max(d, 0.05f) * texel;
        f32 vx = flowX / div;
        f32 vz = flowZ / div;
        const f32 speed = std::hypot(vx, vz);
        if (speed > 12.0f) {
            vx *= 12.0f / speed;
            vz *= 12.0f / speed;
        }
        out.velX[i] = vx;
        out.velZ[i] = vz;
    }
}

WaterSimState preRollWindow(const GridSpec& spec, const HeightFn& height,
                            const WaterSimParams& params,
                            const vector<WaterSource>& sources) {
    WaterSimState state;
    state.spec = spec;
    fillTerrain(spec, height, state.terrain);
    // Offline multigrid to (approximate) equilibrium at the validated
    // Courant point — a few seconds of worker time replacing minutes
    // of visible filling. dryThreshold 0: the live state keeps films.
    terraingen::WaterSolveParams solve;
    solve.rainRate = params.rainRate;
    solve.evaporationRate = params.evaporationRate;
    solve.seaLevel = params.seaLevel;
    solve.dt = 0.05f * std::sqrt(spec.texelSize / 2.0f);
    solve.friction = params.friction;
    solve.dryThreshold = 0.0f;
    solve.multigrid = true;
    solve.maxIterations = 4000;
    solve.fineIterations = 1500;
    const terraingen::WaterSolveResult settled =
        terraingen::solveSteadyWater(spec, state.terrain, solve,
                                     &sources);
    const size_t cells = spec.cells();
    state.depth = settled.depth;
    state.fE.assign(cells, 0.0f);
    state.fW.assign(cells, 0.0f);
    state.fS.assign(cells, 0.0f);
    state.fN.assign(cells, 0.0f);
    state.headBuf.assign(cells, 0.0f);
    state.scratch.assign(cells, 0.0f);
    pinSea(state, state.depth, params.seaLevel);
    zeroBorderWalls(state);
    return state;
}

WaterSimSample sampleSnapshot(const WaterSimSnapshot& snap, f32 x,
                              f32 z) {
    WaterSimSample out;
    const GridSpec& spec = snap.spec;
    if (spec.n < 2 || snap.depth.size() != spec.cells()) {
        return out;
    }
    const f32 margin =
        static_cast<f32>(snap.marginCells) * spec.texelSize;
    const f32 span = static_cast<f32>(spec.n - 1) * spec.texelSize;
    if (x < spec.originX + margin || z < spec.originZ + margin ||
        x > spec.originX + span - margin ||
        z > spec.originZ + span - margin) {
        return out;
    }
    const f32 u = glm::clamp((x - spec.originX) / spec.texelSize, 0.0f,
                             static_cast<f32>(spec.n - 1));
    const f32 v = glm::clamp((z - spec.originZ) / spec.texelSize, 0.0f,
                             static_cast<f32>(spec.n - 1));
    const u32 u0 = glm::min(static_cast<u32>(u), spec.n - 2);
    const u32 v0 = glm::min(static_cast<u32>(v), spec.n - 2);
    const f32 tu = u - static_cast<f32>(u0);
    const f32 tv = v - static_cast<f32>(v0);
    const f32 w[4] = { (1.0f - tu) * (1.0f - tv), tu * (1.0f - tv),
                       (1.0f - tu) * tv, tu * tv };
    const size_t at[4] = { static_cast<size_t>(v0) * spec.n + u0,
                           static_cast<size_t>(v0) * spec.n + u0 + 1,
                           static_cast<size_t>(v0 + 1) * spec.n + u0,
                           static_cast<size_t>(v0 + 1) * spec.n + u0 +
                               1 };
    f32 wetWeight = 0.0f;
    for (i32 k = 0; k < 4; ++k) {
        const size_t i = at[k];
        const f32 d = snap.depth[i];
        out.depth += w[k] * d;
        if (d > 0.0f) {
            wetWeight += w[k];
            out.surface += w[k] * snap.surface[i];
            out.velocityX += w[k] * snap.velX[i];
            out.velocityZ += w[k] * snap.velZ[i];
        }
    }
    if (wetWeight > 0.0f) {
        const f32 inv = 1.0f / wetWeight;
        out.surface *= inv;
        out.velocityX *= inv;
        out.velocityZ *= inv;
    }
    return out;
}

} // namespace render::terrain
