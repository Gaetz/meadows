#include "engine/terrain/generation/ThermalErosion.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace render::terraingen {

namespace {

} // namespace

ThermalResult erodeThermal(const GridSpec& spec, const vector<f32>& height,
                           const ThermalParams& params,
                           const vector<f32>* talusScale,
                           const std::atomic<bool>* cancel) {
    const i32 n = static_cast<i32>(spec.n);
    ThermalResult out;
    out.height = height;
    out.deposit.assign(spec.cells(), 0.0f);
    vector<f32> delta(spec.cells());
    for (i32 iter = 0; iter < params.iterations; ++iter) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            break; // shutdown: partial surface, caller discards
        }
        std::fill(delta.begin(), delta.end(), 0.0f);
        const vector<f32>& h = out.height; // Jacobi: read h, write delta
        for (i32 cz = 0; cz < n; ++cz) {
            for (i32 cx = 0; cx < n; ++cx) {
                const size_t i = static_cast<size_t>(cz) * spec.n + cx;
                if (h[i] <= params.seaLevel) {
                    continue; // base level never sheds
                }
                // Lowest neighbour and its drop.
                size_t low = i;
                f32 lowH = h[i];
                f32 lowDist = spec.texelSize;
                for (const Neighbour& nb : kNeighbours8) {
                    const i32 px = cx + nb.dx;
                    const i32 pz = cz + nb.dz;
                    if (px < 0 || pz < 0 || px >= n || pz >= n) {
                        continue;
                    }
                    const size_t p = static_cast<size_t>(pz) * spec.n +
                                     static_cast<size_t>(px);
                    if (h[p] < lowH) {
                        lowH = h[p];
                        low = p;
                        lowDist = nb.dist * spec.texelSize;
                    }
                }
                if (low == i) {
                    continue;
                }
                const f32 scale = talusScale ? (*talusScale)[i] : 1.0f;
                const f32 talus = params.talusTan * scale * lowDist;
                const f32 drop = h[i] - lowH;
                if (drop <= talus) {
                    continue;
                }
                // Half the excess, damped by rate — Jacobi stability.
                const f32 move = 0.5f * params.rate * (drop - talus);
                delta[i] -= move;
                delta[low] += move;
                out.deposit[low] += move;
            }
        }
        for (size_t i = 0; i < delta.size(); ++i) {
            out.height[i] += delta[i];
        }
    }
    return out;
}

vector<f32> roundRidges(const GridSpec& spec, const vector<f32>& height,
                        const RidgeRoundParams& params,
                        const vector<f32>* weight) {
    if (params.strength <= 0.0f) {
        return height;
    }
    const i32 n = static_cast<i32>(spec.n);
    const i32 r = glm::max(
        1, static_cast<i32>(std::lround(params.radius / spec.texelSize)));
    // Separable edge-clamped box mean — the neighbourhood surface the
    // crests relax toward.
    vector<f32> tmp(spec.cells());
    vector<f32> mean(spec.cells());
    for (i32 z = 0; z < n; ++z) {
        for (i32 x = 0; x < n; ++x) {
            f32 sum = 0.0f;
            i32 count = 0;
            for (i32 d = -r; d <= r; ++d) {
                const i32 px = x + d;
                if (px < 0 || px >= n) {
                    continue;
                }
                sum += height[static_cast<size_t>(z) * spec.n + px];
                ++count;
            }
            tmp[static_cast<size_t>(z) * spec.n + x] =
                sum / static_cast<f32>(count);
        }
    }
    for (i32 z = 0; z < n; ++z) {
        for (i32 x = 0; x < n; ++x) {
            f32 sum = 0.0f;
            i32 count = 0;
            for (i32 d = -r; d <= r; ++d) {
                const i32 pz = z + d;
                if (pz < 0 || pz >= n) {
                    continue;
                }
                sum += tmp[static_cast<size_t>(pz) * spec.n + x];
                ++count;
            }
            mean[static_cast<size_t>(z) * spec.n + x] =
                sum / static_cast<f32>(count);
        }
    }
    vector<f32> out = height;
    for (size_t i = 0; i < out.size(); ++i) {
        if (height[i] <= params.seaLevel) {
            continue; // base level: the coast profile is not ours to shave
        }
        const f32 prominence = height[i] - mean[i];
        if (prominence <= 0.0f) {
            continue; // concave or flat: valleys keep their carve
        }
        f32 w = params.strength *
                glm::smoothstep(0.5f * params.threshold,
                                1.5f * params.threshold, prominence);
        if (weight) {
            w *= glm::clamp((*weight)[i], 0.0f, 1.0f);
        }
        out[i] = height[i] - w * prominence;
    }
    return out;
}

} // namespace render::terraingen
