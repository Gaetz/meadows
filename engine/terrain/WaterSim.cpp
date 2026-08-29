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
}

// Pinned reservoirs (baked lakes): the surface is HELD at the level —
// absorb and supply. The supply is bounded on the way OUT instead
// (reservoirOutflow in updatePipes), so holding the level here stays
// safe.
void applyPins(const WaterSimState& state, vector<f32>& depth) {
    const size_t cells = state.spec.cells();
    if (state.pinned.size() != cells) {
        return;
    }
    for (size_t i = 0; i < cells; ++i) {
        const f32 level = state.pinned[i];
        if (level > kWaterInfoDry + 1.0f) {
            depth[i] = glm::max(0.0f, level - state.terrain[i]);
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
    // Start DRY (sea pin only). The old priority-flood warm start
    // filled EVERY window-enclosed basin to its pass — a mountain
    // valley whose true exit lies past the window rim became a 138 m
    // phantom lake (9.5M m³, replay-measured) draining down the
    // hillsides. Standing water is the BAKE's authority: lakes arrive
    // through the pins, rivers through the sources — the sim only
    // moves water, it never invents it.
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
    const vector<f32>& pinned = state.pinned;
    const bool hasPins = pinned.size() == cells;
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
            // PINNED cells additionally cap at their weir rate: held
            // by an infinite reservoir, their 25% would be meters of
            // column per substep (the 6,200 m³/s replay measurement).
            f32 held = depth[i] * cellArea * 0.25f;
            if (hasPins && pinned[i] > kWaterInfoDry + 1.0f) {
                held = glm::min(held,
                                params.reservoirOutflow * params.dt);
            }
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
        applyPins(state, depth);
        // After the pin (a pinned source cell swallows its discharge).
        // Spread over a small DISC: dumped into one 2 m cell, a 45
        // m³/s source out-raced the capped outflow and dug a ~6 m
        // standing spike — a growing dark spot at every river entry
        // (measured in-game).
        for (const WaterSource& source : sources) {
            const i32 col = static_cast<i32>(
                std::lround((source.x - spec.originX) / texel));
            const i32 row = static_cast<i32>(
                std::lround((source.z - spec.originZ) / texel));
            i32 taps = 0;
            for (i32 dz = -2; dz <= 2; ++dz) {
                for (i32 dx = -2; dx <= 2; ++dx) {
                    if (dx * dx + dz * dz > 4) {
                        continue;
                    }
                    const i32 c = col + dx;
                    const i32 r = row + dz;
                    if (c >= 0 && r >= 0 && c < n && r < n &&
                        terrain[at(c, r)] >= params.seaLevel) {
                        ++taps;
                    }
                }
            }
            if (taps == 0) {
                continue;
            }
            const f32 share =
                source.discharge * params.dt / cellArea /
                static_cast<f32>(taps);
            for (i32 dz = -2; dz <= 2; ++dz) {
                for (i32 dx = -2; dx <= 2; ++dx) {
                    if (dx * dx + dz * dz > 4) {
                        continue;
                    }
                    const i32 c = col + dx;
                    const i32 r = row + dz;
                    if (c >= 0 && r >= 0 && c < n && r < n &&
                        terrain[at(c, r)] >= params.seaLevel) {
                        depth[at(c, r)] += share;
                    }
                }
            }
        }
    }
}

void scrollWindow(WaterSimState& state, i32 dCol, i32 dRow,
                  const HeightFn& height, f32 seaLevel,
                  const vector<sptr<const WaterSimState>>* crumbs) {
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
    if (state.wetMask.size() != state.spec.cells()) {
        state.wetMask.assign(state.spec.cells(), 0);
    }
    // The pinned plane shifts with the rest; the caller rebuilds it
    // via pinLakes after every scroll (whole-plane, cheap), so the
    // entered strips never keep stale pins for long. The wetMask MUST
    // shift too, or the render hysteresis reads misaligned cells after
    // every scroll (flicker).
    vector<f32>* planes[] = { &state.terrain, &state.depth,  &state.fE,
                              &state.fW,      &state.fS,     &state.fN,
                              &state.pinned };
    const auto at = [&](i32 col, i32 row) {
        return static_cast<size_t>(row) * state.spec.n +
               static_cast<size_t>(col);
    };
    // Breadcrumb refill for an ENTERED cell: copy the remembered water
    // (depth, pipes, wet memory — NOT terrain, which is freshly
    // sampled, and NOT pins, which the caller re-rasterizes) from the
    // newest crumb covering the cell's world position. Called AFTER
    // the dry reset; leaves the cell dry when no crumb covers it.
    const auto refillFromCrumbs = [&](size_t i, i32 col, i32 row) {
        if (!crumbs) {
            return;
        }
        const f32 texel = state.spec.texelSize;
        const f32 x = state.spec.x(static_cast<u32>(col));
        const f32 z = state.spec.z(static_cast<u32>(row));
        for (auto it = crumbs->rbegin(); it != crumbs->rend(); ++it) {
            const WaterSimState& crumb = **it;
            if (std::abs(crumb.spec.texelSize - texel) > 1.0e-3f) {
                continue;
            }
            const i32 cc = static_cast<i32>(
                std::lround((x - crumb.spec.originX) / texel));
            const i32 cr = static_cast<i32>(
                std::lround((z - crumb.spec.originZ) / texel));
            if (cc < 0 || cr < 0 ||
                cc >= static_cast<i32>(crumb.spec.n) ||
                cr >= static_cast<i32>(crumb.spec.n)) {
                continue;
            }
            const size_t j =
                static_cast<size_t>(cr) * crumb.spec.n +
                static_cast<size_t>(cc);
            state.depth[i] = crumb.depth[j];
            state.fE[i] = crumb.fE[j];
            state.fW[i] = crumb.fW[j];
            state.fS[i] = crumb.fS[j];
            state.fN[i] = crumb.fN[j];
            if (j < crumb.wetMask.size() &&
                i < state.wetMask.size()) {
                state.wetMask[i] = crumb.wetMask[j];
            }
            return;
        }
    };

    // Entered strips start DRY (terrain sampled, water/pipes/wet
    // memory zeroed). The old "flood bounded by the survivor edge"
    // sweep INVENTED water: flying past a pinned lake painted its
    // level across every entered strip — 20 m of flood over whole
    // valleys (replay-measured, 3.9M m³). Doctrine: the sim moves
    // water, it never invents it — the caller's pinLakes re-rasterizes
    // baked lakes into the strips right after, sources refill the
    // rivers, and the settle margin hides the transition.
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
        {
            u8* mask = state.wetMask.data();
            for (i32 row = 0; row < n; ++row) {
                u8* r = mask + at(0, row);
                if (dCol > 0) {
                    std::memmove(r, r + dCol,
                                 static_cast<size_t>(keep));
                } else {
                    std::memmove(r - dCol, r,
                                 static_cast<size_t>(keep));
                }
            }
        }
        state.spec.originX +=
            static_cast<f32>(dCol) * state.spec.texelSize;
        const i32 firstNew = dCol > 0 ? keep : 0;
        const i32 lastNew = dCol > 0 ? n - 1 : std::abs(dCol) - 1;
        for (i32 row = 0; row < n; ++row) {
            for (i32 col = firstNew; col <= lastNew; ++col) {
                const size_t i = at(col, row);
                state.terrain[i] = height(
                    state.spec.x(static_cast<u32>(col)),
                    state.spec.z(static_cast<u32>(row)));
                state.depth[i] = 0.0f;
                state.fE[i] = 0.0f;
                state.fW[i] = 0.0f;
                state.fS[i] = 0.0f;
                state.fN[i] = 0.0f;
                state.pinned[i] = kWaterInfoDry;
                state.wetMask[i] = 0;
                refillFromCrumbs(i, col, row);
            }
        }
    }

    // --- Z shift, whole-block move.
    if (dRow != 0) {
        const i32 keep = n - std::abs(dRow);
        const size_t rowFloats = static_cast<size_t>(n);
        for (vector<f32>* plane : planes) {
            f32* data = plane->data();
            if (dRow > 0) {
                std::memmove(data, data + at(0, dRow),
                             static_cast<size_t>(keep) * rowFloats *
                                 sizeof(f32));
            } else {
                std::memmove(data + at(0, -dRow), data,
                             static_cast<size_t>(keep) * rowFloats *
                                 sizeof(f32));
            }
        }
        {
            u8* mask = state.wetMask.data();
            if (dRow > 0) {
                std::memmove(mask, mask + at(0, dRow),
                             static_cast<size_t>(keep) * rowFloats);
            } else {
                std::memmove(mask + at(0, -dRow), mask,
                             static_cast<size_t>(keep) * rowFloats);
            }
        }
        state.spec.originZ +=
            static_cast<f32>(dRow) * state.spec.texelSize;
        const i32 firstNew = dRow > 0 ? keep : 0;
        const i32 lastNew = dRow > 0 ? n - 1 : std::abs(dRow) - 1;
        for (i32 row = firstNew; row <= lastNew; ++row) {
            for (i32 col = 0; col < n; ++col) {
                const size_t i = at(col, row);
                state.terrain[i] = height(
                    state.spec.x(static_cast<u32>(col)),
                    state.spec.z(static_cast<u32>(row)));
                state.depth[i] = 0.0f;
                state.fE[i] = 0.0f;
                state.fW[i] = 0.0f;
                state.fS[i] = 0.0f;
                state.fN[i] = 0.0f;
                state.pinned[i] = kWaterInfoDry;
                state.wetMask[i] = 0;
                refillFromCrumbs(i, col, row);
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
    // Per-cell seed target (max covered lake level), then a BFS from
    // the pins: only basin cells CONNECTED to the pinned core get
    // seeded — a mask overhang patch on a downhill slope is
    // disconnected and stays dry (seeded blindly, those became 15 m
    // water TOWERS refilled at every scroll, measured in-game).
    vector<f32>& seedLevel = state.headBuf; // scratch reuse
    seedLevel.assign(cells, kWaterInfoDry);
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
        bool pinnedAny = false;
        size_t deepest = 0;
        f32 deepestColumn = 0.0f;
        for (i32 row = r0; row <= r1; ++row) {
            for (i32 col = c0; col <= c1; ++col) {
                const size_t i =
                    static_cast<size_t>(row) * spec.n + col;
                if (lake.level <= state.terrain[i] + 0.02f) {
                    continue; // rim/bank cell above the water
                }
                const f32 x = spec.x(static_cast<u32>(col));
                const f32 z = spec.z(static_cast<u32>(row));
                if (!lake.covers(x, z)) {
                    continue;
                }
                // Deepest covered cell FIRST (before the crest
                // guard): it is the narrow-lake fallback PIN — a
                // ledge pond surrounded by drops must keep its
                // lifeline even where the guard blocks seeding.
                const f32 column = lake.level - state.terrain[i];
                if (column > deepestColumn) {
                    deepestColumn = column;
                    deepest = i;
                }
                // Crest-overhang guard: the 8 m mask can overrun an
                // ERODED edge by up to one mask texel (4 sim cells),
                // and seeding those plants a full-height water column
                // jutting past the crest above the falls (the "lake
                // cube", measured dev, seen from above). The void
                // past the crest = ground far below the level and
                // OUTSIDE THE LAKE'S BBOX — a mere mask gap will not
                // do: rasterization holes, concave bays and island
                // rings sit over deep water INSIDE the bbox, and
                // keying the guard on coverage seeded a film crater
                // (a 4-cell disc showing the floor) around every such
                // texel — circles at the freshly scrolled margins,
                // measured dev. Such a cell gets the connective film
                // and no pin: the pour carves its real profile.
                {
                    bool pastCrest = false;
                    for (i32 dz = -4; dz <= 4 && !pastCrest; ++dz) {
                        for (i32 dx = -4; dx <= 4; ++dx) {
                            const i32 nc = col + dx;
                            const i32 nr = row + dz;
                            if (nc < 0 || nr < 0 ||
                                nc >= static_cast<i32>(spec.n) ||
                                nr >= static_cast<i32>(spec.n)) {
                                continue;
                            }
                            const size_t nj =
                                static_cast<size_t>(nr) * spec.n +
                                static_cast<size_t>(nc);
                            if (state.terrain[nj] >=
                                lake.level - 2.0f) {
                                continue;
                            }
                            const f32 nx =
                                spec.x(static_cast<u32>(nc));
                            const f32 nz =
                                spec.z(static_cast<u32>(nr));
                            if (nx >= lake.minX && nx <= lake.maxX &&
                                nz >= lake.minZ && nz <= lake.maxZ) {
                                continue;
                            }
                            // Beyond THIS piece's bbox — but a big
                            // lake is baked as PER-TILE pieces: the
                            // ground past a tile seam is the same
                            // lake, not the void. A sibling piece at
                            // the same level covering the point keeps
                            // the cell in the basin (without this,
                            // every interior tile seam grew a 16 m
                            // film band — "the floor rising as if
                            // the lake ended", measured dev).
                            bool sibling = false;
                            for (const LakeSurface& other :
                                 bodies.lakes) {
                                if (&other == &lake) {
                                    continue;
                                }
                                if (glm::abs(other.level - lake.level) <
                                        1.0f &&
                                    other.covers(nx, nz)) {
                                    sibling = true;
                                    break;
                                }
                            }
                            if (!sibling) {
                                pastCrest = true;
                                break;
                            }
                        }
                    }
                    if (pastCrest) {
                        // Interpolate, don't cut (dev feedback: the
                        // hard skip ended the lake short of the crest
                        // with a dry gap before the fall): seed a
                        // thin connective FILM following the terrain
                        // over the crest — publishable, cube-free —
                        // and let the pour carve its real profile.
                        // Never pin past the crest.
                        seedLevel[i] = glm::max(
                            seedLevel[i], state.terrain[i] + 0.08f);
                        continue;
                    }
                }
                // Seed candidate (validated by the pin-connectivity
                // BFS below).
                seedLevel[i] = glm::max(seedLevel[i], lake.level);
                // PIN the mask INTERIOR only (eroded by one mask
                // texel): the 8 m mask rasterized at sim resolution
                // overhangs its banks, and an over-hanging pin is an
                // artesian spring pouring on the hillside forever
                // (water appearing UPHILL of the lake, measured
                // in-game). The interior core supplies; the rim ring
                // lives on seeding + dynamics.
                const f32 mt = lake.maskTexel;
                if (!lake.covers(x - mt, z) || !lake.covers(x + mt, z) ||
                    !lake.covers(x, z - mt) || !lake.covers(x, z + mt)) {
                    continue;
                }
                state.pinned[i] =
                    glm::max(state.pinned[i], lake.level);
                pinnedAny = true;
            }
        }
        // A NARROW lake (under ~3 mask texels) has no interior at all
        // after the erosion: no pin, no BFS root, no seed — it filled
        // at river speed, the remaining "flan" (measured at the spawn
        // cascade). Fall back to pinning its deepest covered cell so
        // the basin seeds and the reservoir supplies.
        if (!pinnedAny && deepestColumn > 0.02f) {
            state.pinned[deepest] =
                glm::max(state.pinned[deepest], lake.level);
        }
    }
    // Seed by CONNECTIVITY: BFS from the pinned core through covered
    // basin cells (terrain below the level) — the lake occupies its
    // whole baked footprint immediately (the "flan" fix), and a mask
    // overhang disconnected from the core never receives a drop.
    vector<u32> queue;
    vector<u8> visited(cells, 0);
    queue.reserve(1024);
    for (size_t i = 0; i < cells; ++i) {
        if (state.pinned[i] > kWaterInfoDry + 1.0f) {
            visited[i] = 1;
            queue.push_back(static_cast<u32>(i));
        }
    }
    const i32 n = static_cast<i32>(spec.n);
    while (!queue.empty()) {
        const u32 i = queue.back();
        queue.pop_back();
        const i32 col = static_cast<i32>(i % spec.n);
        const i32 row = static_cast<i32>(i / spec.n);
        const i32 dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 },
                                 { 0, -1 } };
        for (const auto& d : dirs) {
            const i32 nc = col + d[0];
            const i32 nr = row + d[1];
            if (nc < 0 || nr < 0 || nc >= n || nr >= n) {
                continue;
            }
            const size_t j =
                static_cast<size_t>(nr) * spec.n + static_cast<size_t>(nc);
            if (visited[j]) {
                continue;
            }
            const f32 level = seedLevel[j];
            if (level <= kWaterInfoDry + 1.0f ||
                state.terrain[j] >= level - 0.02f) {
                continue;
            }
            visited[j] = 1;
            queue.push_back(static_cast<u32>(j));
            if (state.depth[j] < 0.01f) {
                state.depth[j] = level - state.terrain[j];
            }
        }
    }
}

void extractSnapshot(WaterSimState& state, const WaterSimParams& params,
                     WaterSimSnapshot& out) {
    const GridSpec& spec = state.spec;
    const size_t cells = spec.cells();
    const u32 n = spec.n;
    out.spec = spec;
    out.marginCells = params.marginCells;
    out.surface.assign(cells, kWaterInfoDry);
    out.depth.assign(cells, 0.0f);
    out.velX.assign(cells, 0.0f);
    out.velZ.assign(cells, 0.0f);
    out.display.resize(cells);
    out.meshVerts.clear();
    out.meshIndices.clear();
    const f32 texel = spec.texelSize;
    if (state.wetMask.size() != cells) {
        state.wetMask.assign(cells, 0);
    }

    // --- Pass 1: HYSTERETIC wetness (docs/WATER-RENDER.md §1.3). A
    // cell turns wet above the high threshold and only dries below the
    // low one — cells hovering at one threshold blinked whole surfaces
    // out per tick. Fast films (a waterfall face runs centimeters)
    // publish at reduced thresholds.
    for (size_t i = 0; i < cells; ++i) {
        const f32 d = state.depth[i];
        const bool wasWet = state.wetMask[i] != 0;
        bool wet = d > (wasWet ? 0.012f : 0.03f);
        if (!wet && d > (wasWet ? 0.0025f : 0.005f)) {
            const f32 fx = state.fE[i] - state.fW[i];
            const f32 fz = state.fS[i] - state.fN[i];
            const f32 div = glm::max(d, 0.05f) * texel;
            wet = (fx * fx + fz * fz) / (div * div) > 1.5f * 1.5f;
        }
        // Sea-pinned: the ocean sheet's territory, published dry.
        if (wet && state.terrain[i] < params.seaLevel &&
            state.terrain[i] + d <= params.seaLevel + 0.01f) {
            wet = false;
        }
        // (Lakes PUBLISH — the sim mesh is the lake inside the rect.
        // A "publish lakes dry, render the baked sheet everywhere"
        // variant was tried and reverted: the rasterized baked sheet
        // OVERHANGS the fall lip — a surface floating over the void
        // at every lake outlet, measured in-game. The sim mesh hugs
        // the real water; the window-boundary continuity is the
        // SHADER's job — identical recipes and a lake-for-lake
        // handover at the rim.)
        state.wetMask[i] = wet ? 1 : 0;
    }
    // --- Pass 2: connectivity (EIGHT-way — a thread descends a cliff
    // diagonally; the 4-way test erased whole waterfalls). Isolated
    // wet cells are transient spray and lose their wet memory.
    {
        vector<f32>& keepBuf = state.headBuf; // scratch reuse
        for (size_t i = 0; i < cells; ++i) {
            keepBuf[i] = 0.0f;
            if (!state.wetMask[i]) {
                continue;
            }
            const i32 col = static_cast<i32>(i % n);
            const i32 row = static_cast<i32>(i / n);
            for (i32 dz = -1; dz <= 1 && keepBuf[i] == 0.0f; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    const i32 nc = col + dx;
                    const i32 nr = row + dz;
                    if (nc >= 0 && nr >= 0 && nc < static_cast<i32>(n) &&
                        nr < static_cast<i32>(n) &&
                        state.wetMask[static_cast<size_t>(nr) * n +
                                      static_cast<size_t>(nc)]) {
                        keepBuf[i] = 1.0f;
                        break;
                    }
                }
            }
        }
        for (size_t i = 0; i < cells; ++i) {
            if (state.wetMask[i] && keepBuf[i] == 0.0f) {
                state.wetMask[i] = 0;
            }
        }
    }
    // --- Pass 3: publish fields for the retained cells.
    for (size_t i = 0; i < cells; ++i) {
        const f32 d = state.depth[i];
        out.display[i] = state.terrain[i] - 0.25f; // dry (texture path)
        if (!state.wetMask[i]) {
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

    // --- Pass 4: the ONE closed mesh — MARCHING SQUARES on the DUAL
    // grid (docs/WATER-RENDER.md §2). Samples live at CELL CENTERS
    // (wetness, surface, depth); every 2x2 block of cells is a dual
    // cell whose wet corners pick one of the 16 contour cases. Tops
    // cover the wet-region polygon; ONE wall follows every contour
    // segment down to the column-capped bottom. The shoreline is a
    // 45-degree-chamfered polyline refined sub-texel by depth
    // interpolation — never the axis-aligned 2 m staircase — and the
    // mesh NEVER leaves the wet cells' footprint (the From Dust
    // intersection ring put geometry on dry slopes and climbed
    // hillsides — reverted, see the ledger). Extent still lives in
    // this geometry: the shader has no dryness discard at all.
    {
        const u32 dn = n - 1;
        const auto cellAt = [&](u32 c, u32 r) {
            return static_cast<size_t>(r) * n + c;
        };
        const auto wetAt = [&](u32 c, u32 r) {
            return state.wetMask[cellAt(c, r)] != 0;
        };
        const auto pushVert = [&](f32 x, f32 y, f32 z, f32 u, f32 v) {
            out.meshVerts.push_back(x);
            out.meshVerts.push_back(y);
            out.meshVerts.push_back(z);
            out.meshVerts.push_back(u);
            out.meshVerts.push_back(v);
            return static_cast<u32>(out.meshVerts.size() / 5 - 1);
        };
        const auto centerU = [&](u32 c) {
            return (static_cast<f32>(c) + 0.5f) / static_cast<f32>(n);
        };
        const auto centerV = [&](u32 r) {
            return (static_cast<f32>(r) + 0.5f) / static_cast<f32>(n);
        };
        vector<u32> centerSlot(cells, ~0u);
        const auto centerVert = [&](u32 c, u32 r) {
            u32& slot = centerSlot[cellAt(c, r)];
            if (slot == ~0u) {
                slot = pushVert(spec.x(c), out.surface[cellAt(c, r)],
                                spec.z(r), centerU(c), centerV(r));
            }
            return slot;
        };
        // A cut dual edge (exactly one wet end) carries one contour
        // vertex, interpolated from the WET center toward the dry one
        // by the depth (iso at ~1.5 cm, clamped [0.2, 0.8] so shore
        // triangles never degenerate). The vertex sits at the WET
        // surface height — the sheet stays level to its edge, the
        // wall drops from there.
        struct EdgeV {
            u32 slot;
            f32 x, z, top, bottom;
        };
        const auto edgeInfo = [&](u32 pc, u32 pr, u32 qc, u32 qr) {
            const size_t pi = cellAt(pc, pr);
            const bool pWet = state.wetMask[pi] != 0;
            const u32 wc = pWet ? pc : qc;
            const u32 wr = pWet ? pr : qr;
            const u32 dc = pWet ? qc : pc;
            const u32 dr = pWet ? qr : pr;
            const size_t wi = cellAt(wc, wr);
            const size_t di = cellAt(dc, dr);
            const f32 d = out.depth[wi];
            const f32 t = glm::clamp(
                d > 1.0e-4f ? 1.0f - 0.015f / d : 0.5f, 0.2f, 0.8f);
            EdgeV ev;
            ev.x = glm::mix(spec.x(wc), spec.x(dc), t);
            ev.z = glm::mix(spec.z(wr), spec.z(dr), t);
            ev.top = out.surface[wi];
            const f32 ground =
                glm::mix(state.terrain[wi], state.terrain[di], t);
            ev.bottom = glm::max(ground - 0.15f,
                                 out.surface[wi] - out.depth[wi] -
                                     0.15f);
            ev.slot = ~0u;
            return ev;
        };
        // Shared contour-vertex slots: one per cut dual edge.
        // Horizontal edges join (c,r)-(c+1,r); vertical (c,r)-(c,r+1).
        vector<u32> hSlot(static_cast<size_t>(dn) * n, ~0u);
        vector<u32> vSlot(static_cast<size_t>(n) * dn, ~0u);
        const auto hEdge = [&](u32 c, u32 r) {
            EdgeV ev = edgeInfo(c, r, c + 1, r);
            u32& slot = hSlot[static_cast<size_t>(r) * dn + c];
            if (slot == ~0u) {
                slot = pushVert(
                    ev.x, ev.top, ev.z,
                    glm::mix(centerU(c), centerU(c + 1),
                             (ev.x - spec.x(c)) / texel),
                    centerV(r));
            }
            ev.slot = slot;
            return ev;
        };
        const auto vEdge = [&](u32 c, u32 r) {
            EdgeV ev = edgeInfo(c, r, c, r + 1);
            u32& slot = vSlot[static_cast<size_t>(r) * n + c];
            if (slot == ~0u) {
                slot = pushVert(
                    ev.x, ev.top, ev.z, centerU(c),
                    glm::mix(centerV(r), centerV(r + 1),
                             (ev.z - spec.z(r)) / texel));
            }
            ev.slot = slot;
            return ev;
        };
        const auto wall = [&](const EdgeV& a, const EdgeV& b, u32 c,
                              u32 r) {
            if (a.bottom >= a.top - 0.01f &&
                b.bottom >= b.top - 0.01f) {
                return; // degenerate sliver on flat ground
            }
            const f32 u = centerU(c);
            const f32 v = centerV(r);
            const u32 ta = pushVert(a.x, a.top, a.z, u, v);
            const u32 tb = pushVert(b.x, b.top, b.z, u, v);
            const u32 ba = pushVert(a.x, a.bottom, a.z, u, v);
            const u32 bb = pushVert(b.x, b.bottom, b.z, u, v);
            for (const u32 k : { ta, tb, bb, ta, bb, ba }) {
                out.meshIndices.push_back(k);
            }
        };
        const auto tri = [&](u32 a, u32 b, u32 cIdx) {
            out.meshIndices.push_back(a);
            out.meshIndices.push_back(b);
            out.meshIndices.push_back(cIdx);
        };
        for (u32 r = 0; r < dn; ++r) {
            for (u32 c = 0; c < dn; ++c) {
                // Corners: A=(c,r) B=(c+1,r) C=(c+1,r+1) D=(c,r+1).
                const bool wA = wetAt(c, r);
                const bool wB = wetAt(c + 1, r);
                const bool wC = wetAt(c + 1, r + 1);
                const bool wD = wetAt(c, r + 1);
                const u32 mask = (wA ? 1u : 0u) | (wB ? 2u : 0u) |
                                 (wC ? 4u : 0u) | (wD ? 8u : 0u);
                if (mask == 0u) {
                    continue;
                }
                if (mask == 15u) {
                    const u32 a = centerVert(c, r);
                    const u32 b = centerVert(c + 1, r);
                    const u32 cc = centerVert(c + 1, r + 1);
                    const u32 dd = centerVert(c, r + 1);
                    tri(a, b, cc);
                    tri(a, cc, dd);
                    continue;
                }
                switch (mask) {
                case 1u: { // A alone
                    const EdgeV ab = hEdge(c, r);
                    const EdgeV da = vEdge(c, r);
                    tri(centerVert(c, r), ab.slot, da.slot);
                    wall(ab, da, c, r);
                    break;
                }
                case 2u: { // B alone
                    const EdgeV ab = hEdge(c, r);
                    const EdgeV bc = vEdge(c + 1, r);
                    tri(centerVert(c + 1, r), bc.slot, ab.slot);
                    wall(bc, ab, c + 1, r);
                    break;
                }
                case 4u: { // C alone
                    const EdgeV bc = vEdge(c + 1, r);
                    const EdgeV cd = hEdge(c, r + 1);
                    tri(centerVert(c + 1, r + 1), cd.slot, bc.slot);
                    wall(cd, bc, c + 1, r + 1);
                    break;
                }
                case 8u: { // D alone
                    const EdgeV cd = hEdge(c, r + 1);
                    const EdgeV da = vEdge(c, r);
                    tri(centerVert(c, r + 1), da.slot, cd.slot);
                    wall(da, cd, c, r + 1);
                    break;
                }
                case 3u: { // A + B
                    const EdgeV bc = vEdge(c + 1, r);
                    const EdgeV da = vEdge(c, r);
                    const u32 a = centerVert(c, r);
                    const u32 b = centerVert(c + 1, r);
                    tri(a, b, bc.slot);
                    tri(a, bc.slot, da.slot);
                    wall(bc, da, c, r);
                    break;
                }
                case 6u: { // B + C
                    const EdgeV cd = hEdge(c, r + 1);
                    const EdgeV ab = hEdge(c, r);
                    const u32 b = centerVert(c + 1, r);
                    const u32 cc = centerVert(c + 1, r + 1);
                    tri(b, cc, cd.slot);
                    tri(b, cd.slot, ab.slot);
                    wall(cd, ab, c + 1, r);
                    break;
                }
                case 12u: { // C + D
                    const EdgeV da = vEdge(c, r);
                    const EdgeV bc = vEdge(c + 1, r);
                    const u32 cc = centerVert(c + 1, r + 1);
                    const u32 dd = centerVert(c, r + 1);
                    tri(cc, dd, da.slot);
                    tri(cc, da.slot, bc.slot);
                    wall(da, bc, c, r + 1);
                    break;
                }
                case 9u: { // D + A
                    const EdgeV ab = hEdge(c, r);
                    const EdgeV cd = hEdge(c, r + 1);
                    const u32 dd = centerVert(c, r + 1);
                    const u32 a = centerVert(c, r);
                    tri(dd, a, ab.slot);
                    tri(dd, ab.slot, cd.slot);
                    wall(ab, cd, c, r);
                    break;
                }
                case 5u: { // A + C diagonal: kept DISCONNECTED
                    const EdgeV ab = hEdge(c, r);
                    const EdgeV da = vEdge(c, r);
                    tri(centerVert(c, r), ab.slot, da.slot);
                    wall(ab, da, c, r);
                    const EdgeV cd = hEdge(c, r + 1);
                    const EdgeV bc = vEdge(c + 1, r);
                    tri(centerVert(c + 1, r + 1), cd.slot, bc.slot);
                    wall(cd, bc, c + 1, r + 1);
                    break;
                }
                case 10u: { // B + D diagonal: kept DISCONNECTED
                    const EdgeV bc = vEdge(c + 1, r);
                    const EdgeV ab = hEdge(c, r);
                    tri(centerVert(c + 1, r), bc.slot, ab.slot);
                    wall(bc, ab, c + 1, r);
                    const EdgeV da = vEdge(c, r);
                    const EdgeV cd = hEdge(c, r + 1);
                    tri(centerVert(c, r + 1), da.slot, cd.slot);
                    wall(da, cd, c, r + 1);
                    break;
                }
                case 7u: { // D dry
                    const EdgeV cd = hEdge(c, r + 1);
                    const EdgeV da = vEdge(c, r);
                    const u32 a = centerVert(c, r);
                    const u32 b = centerVert(c + 1, r);
                    const u32 cc = centerVert(c + 1, r + 1);
                    tri(a, b, cc);
                    tri(a, cc, cd.slot);
                    tri(a, cd.slot, da.slot);
                    wall(cd, da, c, r);
                    break;
                }
                case 14u: { // A dry
                    const EdgeV ab = hEdge(c, r);
                    const EdgeV da = vEdge(c, r);
                    const u32 b = centerVert(c + 1, r);
                    const u32 cc = centerVert(c + 1, r + 1);
                    const u32 dd = centerVert(c, r + 1);
                    tri(b, cc, dd);
                    tri(b, dd, da.slot);
                    tri(b, da.slot, ab.slot);
                    wall(da, ab, c + 1, r);
                    break;
                }
                case 13u: { // B dry
                    const EdgeV ab = hEdge(c, r);
                    const EdgeV bc = vEdge(c + 1, r);
                    const u32 cc = centerVert(c + 1, r + 1);
                    const u32 dd = centerVert(c, r + 1);
                    const u32 a = centerVert(c, r);
                    tri(cc, dd, a);
                    tri(cc, a, ab.slot);
                    tri(cc, ab.slot, bc.slot);
                    wall(ab, bc, c, r);
                    break;
                }
                case 11u: { // C dry
                    const EdgeV bc = vEdge(c + 1, r);
                    const EdgeV cd = hEdge(c, r + 1);
                    const u32 dd = centerVert(c, r + 1);
                    const u32 a = centerVert(c, r);
                    const u32 b = centerVert(c + 1, r);
                    tri(dd, a, b);
                    tri(dd, b, bc.slot);
                    tri(dd, bc.slot, cd.slot);
                    wall(bc, cd, c, r + 1);
                    break;
                }
                default:
                    break;
                }
            }
        }
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
    // No flood warm start: it invented phantom lakes in every
    // window-enclosed valley (see initWindow). Channels fill from the
    // sources + rain over the burst budget instead.
    solve.warmStart = false;
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
constexpr char kDumpMagic[4] = { 'W', 'S', 'D', '2' };
constexpr char kDumpMagicV1[4] = { 'W', 'S', 'D', '1' };
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
    const bool v2 = std::memcmp(magic, kDumpMagic, 4) == 0;
    const bool v1 = std::memcmp(magic, kDumpMagicV1, 4) == 0;
    read(state.spec.n);
    read(state.spec.originX);
    read(state.spec.originZ);
    read(state.spec.texelSize);
    if (v2) {
        read(params);
    } else {
        // WSD1: the params struct minus the appended weir field.
        file.read(reinterpret_cast<char*>(&params),
                  sizeof(WaterSimParams) - sizeof(f32));
        params.reservoirOutflow = WaterSimParams {}.reservoirOutflow;
    }
    if (!file || (!v2 && !v1) || state.spec.n < 8 ||
        state.spec.n > 4096 || state.spec.texelSize <= 0.0f) {
        LOG_ERROR("loadSimState: not a WSD dump: {}", path);
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

CachedWindowPick chooseCachedWindow(
    const vector<terraingen::GridSpec>& cached,
    const terraingen::GridSpec& target, f32 minOverlap) {
    CachedWindowPick pick;
    f32 best = glm::max(minOverlap, 1.0e-4f);
    const f32 texel = target.texelSize;
    const i32 n = static_cast<i32>(target.n);
    for (size_t c = 0; c < cached.size(); ++c) {
        const terraingen::GridSpec& spec = cached[c];
        if (spec.n != target.n ||
            std::abs(spec.texelSize - texel) > 1.0e-3f) {
            continue; // knob changed since it was cached
        }
        // Origins are texel-snapped, so the shift is (near-)integral.
        const f32 fCol = (target.originX - spec.originX) / texel;
        const f32 fRow = (target.originZ - spec.originZ) / texel;
        const i32 dCol = static_cast<i32>(std::lround(fCol));
        const i32 dRow = static_cast<i32>(std::lround(fRow));
        if (std::abs(fCol - static_cast<f32>(dCol)) > 0.01f ||
            std::abs(fRow - static_cast<f32>(dRow)) > 0.01f) {
            continue; // off-grid (shouldn't happen — be safe)
        }
        const i32 keepX = n - std::abs(dCol);
        const i32 keepZ = n - std::abs(dRow);
        if (keepX <= 0 || keepZ <= 0) {
            continue; // disjoint
        }
        const f32 overlap = static_cast<f32>(keepX) *
                            static_cast<f32>(keepZ) /
                            (static_cast<f32>(n) * static_cast<f32>(n));
        if (overlap > best) {
            best = overlap;
            pick.index = static_cast<i32>(c);
            pick.dCol = dCol;
            pick.dRow = dRow;
        }
    }
    return pick;
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
