#include "engine/terrain/WaterSim.hpp"

#include <cmath>
#include <cstring>
#include <fstream>

#include <glm/glm.hpp>

#include "engine/core/Log.hpp"
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
    // Pinned reservoirs (baked lakes): held level, absorb and supply.
    if (state.pinned.size() == cells) {
        for (size_t i = 0; i < cells; ++i) {
            const f32 level = state.pinned[i];
            if (level > kWaterInfoDry + 1.0f) {
                depth[i] = glm::max(0.0f, level - state.terrain[i]);
            }
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
    state.pinned.assign(cells, kWaterInfoDry);
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
            // Real-time drain cap: at most a QUARTER of the held
            // volume leaves per substep (the offline limiter allows
            // 100%). A cell that fully empties each substep turns a
            // cliff flow into m³ PACKETS hopping one cell per substep
            // — and a pinned reservoir, refilled instantly, machine-
            // gunned them (the accumulating checkerboard of detached
            // quads across the fall, measured in-game). The cap only
            // binds in that regime: gentle flows never drain 25% of a
            // cell in one substep; steep-cell equilibria shift by at
            // most ~0.2 m under storm rain (oracle cross-check bound).
            const f32 held = depth[i] * cellArea * 0.25f;
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
    if (state.pinned.size() != state.spec.cells()) {
        state.pinned.assign(state.spec.cells(), kWaterInfoDry);
    }
    // The pinned plane shifts with the rest; the caller rebuilds it
    // via pinLakes after every scroll (whole-plane, cheap), so the
    // entered strips never keep stale pins for long.
    vector<f32>* planes[] = { &state.terrain, &state.depth,  &state.fE,
                              &state.fW,      &state.fS,     &state.fN,
                              &state.pinned };
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

void pinLakes(WaterSimState& state, const WaterBodies& bodies) {
    const GridSpec& spec = state.spec;
    const size_t cells = spec.cells();
    state.pinned.assign(cells, kWaterInfoDry);
    const f32 texel = spec.texelSize;
    const f32 maxX = spec.originX + static_cast<f32>(spec.n - 1) * texel;
    const f32 maxZ = spec.originZ + static_cast<f32>(spec.n - 1) * texel;
    for (const LakeSurface& lake : bodies.lakes) {
        if (lake.maxX < spec.originX || lake.minX > maxX ||
            lake.maxZ < spec.originZ || lake.minZ > maxZ) {
            continue;
        }
        // Masked (generated) lakes only: a maskless authored pond pins
        // its whole BBOX and would flood slopes the rectangle clips.
        if (lake.mask.empty() || lake.maskWidth == 0) {
            continue;
        }
        const i32 c0 = glm::max(
            static_cast<i32>((lake.minX - spec.originX) / texel), 0);
        const i32 c1 = glm::min(
            static_cast<i32>((lake.maxX - spec.originX) / texel) + 1,
            static_cast<i32>(spec.n) - 1);
        const i32 r0 = glm::max(
            static_cast<i32>((lake.minZ - spec.originZ) / texel), 0);
        const i32 r1 = glm::min(
            static_cast<i32>((lake.maxZ - spec.originZ) / texel) + 1,
            static_cast<i32>(spec.n) - 1);
        for (i32 row = r0; row <= r1; ++row) {
            for (i32 col = c0; col <= c1; ++col) {
                const size_t i =
                    static_cast<size_t>(row) * spec.n + col;
                if (lake.level <= state.terrain[i] + 0.02f) {
                    continue; // rim/bank cell above the water
                }
                // Mask-INTERIOR only (eroded by one mask texel): the
                // 8 m mask rasterized at sim resolution overhangs its
                // banks, and an over-hanging pin is an artesian spring
                // pouring on the hillside forever (water appearing
                // UPHILL of the lake, measured in-game). The interior
                // core still supplies; the true rim overflow is the
                // sim's own job on the fine terrain.
                const f32 x = spec.x(static_cast<u32>(col));
                const f32 z = spec.z(static_cast<u32>(row));
                const f32 mt = lake.maskTexel;
                if (!lake.covers(x, z) || !lake.covers(x - mt, z) ||
                    !lake.covers(x + mt, z) || !lake.covers(x, z - mt) ||
                    !lake.covers(x, z + mt)) {
                    continue;
                }
                state.pinned[i] =
                    glm::max(state.pinned[i], lake.level);
            }
        }
    }
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
    // A cell publishes when its film beats the dry threshold — or when
    // it moves FAST with any substance at all: on a near-vertical face
    // the physical film runs centimeters, and drying it erased whole
    // waterfalls from the render while the sim carried them fine.
    const auto wetAt = [&](size_t j) {
        if (state.depth[j] > params.dryThreshold) {
            return true;
        }
        if (state.depth[j] > 0.004f) {
            const f32 fx = state.fE[j] - state.fW[j];
            const f32 fz = state.fS[j] - state.fN[j];
            const f32 div = glm::max(state.depth[j], 0.05f) * texel;
            return (fx * fx + fz * fz) / (div * div) > 1.5f * 1.5f;
        }
        return false;
    };
    for (size_t i = 0; i < cells; ++i) {
        const f32 d = state.depth[i];
        out.display[i] = state.terrain[i] - 0.25f; // dry tuck default
        if (!wetAt(i)) {
            continue;
        }
        // Sea-pinned: the ocean sheet's territory, read dry.
        if (state.terrain[i] < params.seaLevel &&
            state.terrain[i] + d <= params.seaLevel + 0.01f) {
            continue;
        }
        // An isolated wet cell (no wet neighbour at all) is transient
        // spray, not a water body — rendered, each one made a detached
        // diamond. EIGHT-connected: a thread descending a cliff hops
        // DIAGONALLY cell to cell, and the 4-neighbour test erased
        // whole waterfalls (measured in-game — "only the baked water
        // shows").
        const size_t col = i % spec.n;
        const size_t row = i / spec.n;
        bool connected = false;
        for (i32 dz = -1; dz <= 1 && !connected; ++dz) {
            for (i32 dx = -1; dx <= 1 && !connected; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const i32 nc = static_cast<i32>(col) + dx;
                const i32 nr = static_cast<i32>(row) + dz;
                if (nc >= 0 && nr >= 0 &&
                    nc < static_cast<i32>(spec.n) &&
                    nr < static_cast<i32>(spec.n) &&
                    wetAt(static_cast<size_t>(nr) * spec.n +
                          static_cast<size_t>(nc))) {
                    connected = true;
                }
            }
        }
        if (!connected) {
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
    state.pinned.assign(cells, kWaterInfoDry);
    pinSea(state, state.depth, params.seaLevel);
    zeroBorderWalls(state);
    return state;
}

namespace {
constexpr char kDumpMagic[4] = { 'W', 'S', 'D', '1' };
}

bool dumpSimState(const WaterSimState& state,
                  const WaterSimParams& params,
                  const vector<terraingen::WaterSource>& sources,
                  const char* path) {
    if (!state.valid()) {
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("dumpSimState: cannot open {}", path);
        return false;
    }
    const auto write = [&](const auto& v) {
        file.write(reinterpret_cast<const char*>(&v), sizeof(v));
    };
    const auto writePlane = [&](const vector<f32>& plane) {
        file.write(reinterpret_cast<const char*>(plane.data()),
                   static_cast<std::streamsize>(plane.size() *
                                                sizeof(f32)));
    };
    file.write(kDumpMagic, 4);
    write(state.spec.n);
    write(state.spec.originX);
    write(state.spec.originZ);
    write(state.spec.texelSize);
    write(params);
    const u32 count = static_cast<u32>(sources.size());
    write(count);
    for (const terraingen::WaterSource& s : sources) {
        write(s);
    }
    writePlane(state.terrain);
    writePlane(state.depth);
    writePlane(state.fE);
    writePlane(state.fW);
    writePlane(state.fS);
    writePlane(state.fN);
    const bool hasPins = state.pinned.size() == state.spec.cells();
    const u8 pins = hasPins ? 1 : 0;
    write(pins);
    if (hasPins) {
        writePlane(state.pinned);
    }
    return static_cast<bool>(file);
}

bool loadSimState(const char* path, WaterSimState& state,
                  WaterSimParams& params,
                  vector<terraingen::WaterSource>& sources) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("loadSimState: cannot open {}", path);
        return false;
    }
    const auto read = [&](auto& v) {
        file.read(reinterpret_cast<char*>(&v), sizeof(v));
    };
    char magic[4] = {};
    file.read(magic, 4);
    read(state.spec.n);
    read(state.spec.originX);
    read(state.spec.originZ);
    read(state.spec.texelSize);
    read(params);
    if (!file || std::memcmp(magic, kDumpMagic, 4) != 0 ||
        state.spec.n < 8 || state.spec.n > 4096 ||
        state.spec.texelSize <= 0.0f) {
        LOG_ERROR("loadSimState: not a WSD1 dump: {}", path);
        return false;
    }
    u32 count = 0;
    read(count);
    if (!file || count > 4096) {
        return false;
    }
    sources.resize(count);
    for (terraingen::WaterSource& s : sources) {
        read(s);
    }
    const size_t cells = state.spec.cells();
    const auto readPlane = [&](vector<f32>& plane) {
        plane.resize(cells);
        file.read(reinterpret_cast<char*>(plane.data()),
                  static_cast<std::streamsize>(cells * sizeof(f32)));
    };
    readPlane(state.terrain);
    readPlane(state.depth);
    readPlane(state.fE);
    readPlane(state.fW);
    readPlane(state.fS);
    readPlane(state.fN);
    u8 pins = 0;
    read(pins);
    if (pins != 0) {
        readPlane(state.pinned);
    } else {
        state.pinned.assign(cells, kWaterInfoDry);
    }
    state.headBuf.assign(cells, 0.0f);
    state.scratch.assign(cells, 0.0f);
    return static_cast<bool>(file);
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
