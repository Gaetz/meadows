#include "engine/terrain/FireField.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace render::terrain {

namespace {

void sampleCell(FireGrid& grid, size_t i, i32 col, i32 row, const FuelFn& fuel) {
    if (grid.fuel[i] >= 0.0f) {
        return; // already sampled
    }
    const f32 x = grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize;
    const f32 z = grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize;
    FireCellFuel f;
    if (fuel) {
        f = fuel(x, z);
    }
    grid.fuel[i] = glm::max(f.fuel, 0.0f);
    grid.fuel0[i] = grid.fuel[i];
    grid.flammability[i] = glm::clamp(f.flammability, 0.0f, 1.0f);
    grid.moisture[i] = glm::clamp(f.moisture, 0.0f, 1.0f);
}

} // namespace

void fireInitWindow(FireGrid& grid, const terraingen::GridSpec& spec) {
    grid.spec = spec;
    const size_t cells = spec.cells();
    grid.heat.assign(cells, 0.0f);
    grid.fuel.assign(cells, -1.0f);
    grid.fuel0.assign(cells, 0.0f);
    grid.flammability.assign(cells, 0.0f);
    grid.moisture.assign(cells, 0.0f);
    grid.state.assign(cells, static_cast<u8>(FireState::Dormant));
    grid.ember.assign(cells, 0.0f);
    grid.regrow.assign(cells, 0.0f);
    grid.tick = 0;
}

void fireScrollWindow(FireGrid& grid, i32 dCol, i32 dRow) {
    if (!grid.valid() || (dCol == 0 && dRow == 0)) {
        return;
    }
    const i32 n = static_cast<i32>(grid.spec.n);
    FireGrid next;
    terraingen::GridSpec spec = grid.spec;
    spec.originX += static_cast<f32>(dCol) * spec.texelSize;
    spec.originZ += static_cast<f32>(dRow) * spec.texelSize;
    fireInitWindow(next, spec);
    next.tick = grid.tick;
    for (i32 row = 0; row < n; ++row) {
        const i32 srcRow = row + dRow;
        if (srcRow < 0 || srcRow >= n) {
            continue;
        }
        for (i32 col = 0; col < n; ++col) {
            const i32 srcCol = col + dCol;
            if (srcCol < 0 || srcCol >= n) {
                continue;
            }
            const size_t d = static_cast<size_t>(row) * n + col;
            const size_t s = static_cast<size_t>(srcRow) * n + srcCol;
            next.heat[d] = grid.heat[s];
            next.fuel[d] = grid.fuel[s];
            next.fuel0[d] = grid.fuel0[s];
            next.flammability[d] = grid.flammability[s];
            next.moisture[d] = grid.moisture[s];
            next.state[d] = grid.state[s];
            next.ember[d] = grid.ember[s];
            next.regrow[d] = grid.regrow[s];
        }
    }
    grid = std::move(next);
}

void fireIgnite(FireGrid& grid, f32 x, f32 z, f32 radius, f32 heat,
                const FuelFn& fuel) {
    if (!grid.valid() || heat <= 0.0f) {
        return;
    }
    const i32 n = static_cast<i32>(grid.spec.n);
    const f32 texel = grid.spec.texelSize;
    const i32 col = static_cast<i32>(std::lround((x - grid.spec.originX) / texel));
    const i32 row = static_cast<i32>(std::lround((z - grid.spec.originZ) / texel));
    const i32 reach = static_cast<i32>(std::ceil(radius / texel));
    for (i32 dz = -reach; dz <= reach; ++dz) {
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const i32 c = col + dx;
            const i32 r = row + dz;
            if (c < 0 || r < 0 || c >= n || r >= n) {
                continue;
            }
            const f32 dist = std::sqrt(static_cast<f32>(dx * dx + dz * dz)) * texel;
            if (dist > radius) {
                continue;
            }
            const size_t i = static_cast<size_t>(r) * n + c;
            sampleCell(grid, i, c, r, fuel);
            if (grid.state[i] == static_cast<u8>(FireState::Burnt)) {
                continue;
            }
            grid.heat[i] += heat;
        }
    }
}

void fireDouse(FireGrid& grid, f32 x, f32 z, f32 radius) {
    if (!grid.valid()) {
        return;
    }
    const i32 n = static_cast<i32>(grid.spec.n);
    const f32 texel = grid.spec.texelSize;
    const i32 col = static_cast<i32>(std::lround((x - grid.spec.originX) / texel));
    const i32 row = static_cast<i32>(std::lround((z - grid.spec.originZ) / texel));
    const i32 reach = static_cast<i32>(std::ceil(radius / texel));
    for (i32 dz = -reach; dz <= reach; ++dz) {
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const i32 c = col + dx;
            const i32 r = row + dz;
            if (c < 0 || r < 0 || c >= n || r >= n) {
                continue;
            }
            if (std::sqrt(static_cast<f32>(dx * dx + dz * dz)) * texel > radius) {
                continue;
            }
            const size_t i = static_cast<size_t>(r) * n + c;
            if (grid.state[i] == static_cast<u8>(FireState::Burning)) {
                grid.state[i] = static_cast<u8>(FireState::Dormant);
            }
            grid.heat[i] = 0.0f;
            grid.ember[i] = 0.0f;
        }
    }
}

void fireStep(FireGrid& grid, const FireParams& params, const FuelFn& fuel,
              const WetFn& wet, FireStats* stats, const WindFn& windAt) {
    if (!grid.valid()) {
        return;
    }
    const i32 n = static_cast<i32>(grid.spec.n);
    const f32 dt = params.dt;
    FireStats local;
    // 1. Water first (the shifumi: water beats fire): a wet cell cannot
    //    burn and loses its heat; a burning one is doused.
    if (wet) {
        for (i32 row = 0; row < n; ++row) {
            for (i32 col = 0; col < n; ++col) {
                const size_t i = static_cast<size_t>(row) * n + col;
                if (grid.state[i] == static_cast<u8>(FireState::Burnt)) {
                    if (grid.ember[i] > 0.0f) {
                        const f32 bx = grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize;
                        const f32 bz = grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize;
                        if (wet(bx, bz)) {
                            grid.ember[i] = 0.0f; // quenched
                        }
                    }
                    continue; // stays burnt
                }
                if (grid.heat[i] <= 0.0f &&
                    grid.state[i] != static_cast<u8>(FireState::Burning)) {
                    continue; // nothing to douse, nothing to cool
                }
                const f32 x = grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize;
                const f32 z = grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize;
                if (wet(x, z)) {
                    if (grid.state[i] == static_cast<u8>(FireState::Burning)) {
                        ++local.doused;
                    }
                    grid.state[i] = static_cast<u8>(FireState::Wet);
                    grid.heat[i] = 0.0f;
                    grid.ember[i] = 0.0f;
                }
            }
        }
    }
    // 2. Burning cells deal heat to their neighbours (into a scratch so
    //    the order of visits never matters).
    vector<f32> dealt(grid.cells(), 0.0f);
    // Wind: strength 0..1 = how much of the spread follows the wind's
    // direction (a full wind leaves a quarter of the rate to the sides
    // and the back — the bell-shaped downwind front). Uniform, or per
    // burning cell through windAt (the wind field's gusts).
    static const i32 kOffsets[8][2] = { { 1, 0 },  { -1, 0 }, { 0, 1 },  { 0, -1 },
                                        { 1, 1 },  { -1, 1 }, { 1, -1 }, { -1, -1 } };
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = static_cast<size_t>(row) * n + col;
            if (grid.state[i] != static_cast<u8>(FireState::Burning)) {
                continue;
            }
            const Vec2 wind =
                windAt ? windAt(grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize,
                                grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize)
                       : params.wind;
            const f32 windLen = glm::min(glm::length(wind), 1.0f);
            const Vec2 windDir = windLen > 1e-4f ? wind / glm::length(wind) : Vec2 { 0.0f };
            for (const auto& off : kOffsets) {
                const i32 c = col + off[0];
                const i32 r = row + off[1];
                if (c < 0 || r < 0 || c >= n || r >= n) {
                    continue;
                }
                const size_t j = static_cast<size_t>(r) * n + c;
                if (grid.state[j] != static_cast<u8>(FireState::Dormant)) {
                    continue;
                }
                sampleCell(grid, j, c, r, fuel);
                if (grid.fuel[j] <= 0.0f) {
                    continue; // bare rock, water's edge: nothing to catch
                }
                const Vec2 dir = glm::normalize(Vec2 { static_cast<f32>(off[0]),
                                                       static_cast<f32>(off[1]) });
                const f32 windward =
                    (1.0f - 0.75f * windLen) +
                    0.75f * windLen * glm::max(0.0f, glm::dot(windDir, dir));
                dealt[j] += params.spreadRate * windward * grid.flammability[j] *
                            (1.0f - grid.moisture[j]) * dt;
            }
        }
    }
    // 3. Heat lands; ignitions under the budget; burning consumes fuel;
    //    idle heat decays.
    u32 budget = params.spreadBudgetPerTick;
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = static_cast<size_t>(row) * n + col;
            const auto state = static_cast<FireState>(grid.state[i]);
            switch (state) {
            case FireState::Dormant: {
                grid.heat[i] += dealt[i];
                if (grid.heat[i] >= params.ignitionPoints) {
                    sampleCell(grid, i, col, row, fuel);
                    if (grid.fuel[i] > 0.0f && budget > 0) {
                        grid.state[i] = static_cast<u8>(FireState::Burning);
                        --budget;
                        ++local.ignited;
                    } else if (grid.fuel[i] <= 0.0f) {
                        grid.heat[i] = 0.0f; // nothing to burn: the heat is lost
                    }
                    // Over budget: the heat stays, it ignites next tick.
                } else if (dealt[i] <= 0.0f) {
                    grid.heat[i] = glm::max(0.0f, grid.heat[i] - params.heatDecay * dt);
                }
                break;
            }
            case FireState::Burning:
                grid.fuel[i] -= params.burnRate * dt;
                if (grid.fuel[i] <= 0.0f) {
                    grid.fuel[i] = 0.0f;
                    grid.state[i] = static_cast<u8>(FireState::Burnt);
                    grid.heat[i] = 0.0f;
                    // The embers continue the glow where it stood (no
                    // second front lighting up when the cell burns out).
                    const f32 burnedSeconds =
                        grid.fuel0[i] / glm::max(params.burnRate, 1e-3f);
                    grid.ember[i] = glm::clamp(
                        1.0f - burnedSeconds / glm::max(params.emberSeconds, 0.01f),
                        0.0f, 1.0f);
                    grid.regrow[i] = 0.0f;
                }
                break;
            case FireState::Wet:
                // Stays wet while the water lasts: WetFn re-checked above
                // each tick; when it dries the cell is dormant again.
                if (!wet) {
                    grid.state[i] = static_cast<u8>(FireState::Dormant);
                }
                break;
            case FireState::Burnt: {
                grid.ember[i] = glm::max(
                    0.0f, grid.ember[i] - dt / glm::max(params.emberSeconds, 0.01f));
                // Life returns, the wetter the sooner.
                const f32 rate = (1.0f + 3.0f * grid.moisture[i]) /
                                 glm::max(params.regrowSeconds, 0.01f);
                grid.regrow[i] += dt * rate;
                if (grid.regrow[i] >= 1.0f) {
                    grid.regrow[i] = 0.0f;
                    grid.ember[i] = 0.0f;
                    grid.heat[i] = 0.0f;
                    grid.fuel[i] = grid.fuel0[i]; // fresh fuel: it can burn again
                    grid.state[i] = static_cast<u8>(FireState::Dormant);
                }
                break;
            }
            }
            if (grid.state[i] == static_cast<u8>(FireState::Burning)) {
                ++local.burning;
            } else if (grid.state[i] == static_cast<u8>(FireState::Burnt)) {
                ++local.burnt;
            }
        }
    }
    // Wet cells dry when the water is gone (checked against WetFn).
    if (wet) {
        for (i32 row = 0; row < n; ++row) {
            for (i32 col = 0; col < n; ++col) {
                const size_t i = static_cast<size_t>(row) * n + col;
                if (grid.state[i] != static_cast<u8>(FireState::Wet)) {
                    continue;
                }
                const f32 x = grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize;
                const f32 z = grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize;
                if (!wet(x, z)) {
                    grid.state[i] = static_cast<u8>(FireState::Dormant);
                }
            }
        }
    }
    ++grid.tick;
    if (stats) {
        *stats = local;
    }
}

void fireScorch(const FireGrid& grid, const FireParams& params, vector<u8>& out) {
    out.assign(grid.cells(), 0);
    const f32 frontSeconds = glm::max(params.emberSeconds, 0.01f);
    const f32 burnRate = glm::max(params.burnRate, 1e-3f);
    for (size_t i = 0; i < grid.cells(); ++i) {
        const auto state = static_cast<FireState>(grid.state[i]);
        f32 scorch = 0.0f;
        if (state == FireState::Burnt) {
            scorch = 1.0f - glm::clamp(grid.regrow[i], 0.0f, 1.0f);
        } else if (state == FireState::Burning) {
            const f32 burned = (grid.fuel0[i] - grid.fuel[i]) / burnRate; // seconds
            scorch = glm::clamp(burned / frontSeconds, 0.0f, 1.0f);
        }
        out[i] = static_cast<u8>(glm::clamp(scorch, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

void fireGlow(const FireGrid& grid, const FireParams& params, vector<u8>& out) {
    out.assign(grid.cells(), 0);
    const f32 points = glm::max(params.ignitionPoints, 1e-3f);
    const f32 frontSeconds = glm::max(params.emberSeconds, 0.01f);
    const f32 burnRate = glm::max(params.burnRate, 1e-3f);
    for (size_t i = 0; i < grid.cells(); ++i) {
        const auto state = static_cast<FireState>(grid.state[i]);
        f32 glow = 0.0f;
        if (state == FireState::Burning) {
            const f32 burned = (grid.fuel0[i] - grid.fuel[i]) / burnRate; // seconds
            glow = glm::clamp(1.0f - burned / frontSeconds, 0.0f, 1.0f);
        } else if (state == FireState::Burnt) {
            glow = 0.55f * grid.ember[i]; // the bed cools below the flames
        } else if (state == FireState::Dormant && grid.heat[i] > 0.0f) {
            glow = 0.5f * glm::clamp(grid.heat[i] / points, 0.0f, 1.0f);
        }
        out[i] = static_cast<u8>(glm::clamp(glow, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

vector<Vec2> fireBurningCenters(const FireGrid& grid, u32 maxCount,
                                const FireParams& params, f32 maxBurnedSeconds) {
    struct Hot {
        f32 key;
        Vec2 at;
    };
    vector<Hot> hot;
    const i32 n = static_cast<i32>(grid.spec.n);
    const f32 rate = glm::max(params.burnRate, 1e-3f);
    const f32 points = glm::max(params.ignitionPoints, 1e-3f);
    for (i32 row = 0; row < n; ++row) {
        for (i32 col = 0; col < n; ++col) {
            const size_t i = static_cast<size_t>(row) * n + col;
            const auto state = static_cast<FireState>(grid.state[i]);
            if (state == FireState::Dormant) {
                if (grid.heat[i] >= 0.5f * points && grid.fuel[i] > 0.0f) {
                    // About to catch: a flame already, the least hot.
                    hot.push_back({ -1.0f,
                                    { grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize,
                                      grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize } });
                }
                continue;
            }
            if (state != FireState::Burning) {
                continue;
            }
            if ((grid.fuel0[i] - grid.fuel[i]) / rate > maxBurnedSeconds) {
                continue; // ignited too long ago: behind the front
            }
            // Hottest = the most fuel left to burn (the longest flames).
            hot.push_back({ grid.fuel[i],
                            { grid.spec.originX + static_cast<f32>(col) * grid.spec.texelSize,
                              grid.spec.originZ + static_cast<f32>(row) * grid.spec.texelSize } });
        }
    }
    std::stable_sort(hot.begin(), hot.end(),
                     [](const Hot& a, const Hot& b) { return a.key > b.key; });
    vector<Vec2> out;
    for (size_t i = 0; i < hot.size() && i < maxCount; ++i) {
        out.push_back(hot[i].at);
    }
    return out;
}

} // namespace render::terrain
