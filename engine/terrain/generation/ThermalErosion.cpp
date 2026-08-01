#include "engine/terrain/generation/ThermalErosion.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace render::terraingen {

namespace {

struct Neighbour {
    i32 dx;
    i32 dz;
    f32 dist;
};

constexpr Neighbour kNeighbours[8] = {
    { -1, 0, 1.0f },          { 1, 0, 1.0f },
    { 0, -1, 1.0f },          { 0, 1, 1.0f },
    { -1, -1, 1.41421356f },  { 1, -1, 1.41421356f },
    { -1, 1, 1.41421356f },   { 1, 1, 1.41421356f },
};

} // namespace

ThermalResult erodeThermal(const GridSpec& spec, const vector<f32>& height,
                           const ThermalParams& params,
                           const vector<f32>* talusScale) {
    const i32 n = static_cast<i32>(spec.n);
    ThermalResult out;
    out.height = height;
    out.deposit.assign(spec.cells(), 0.0f);
    vector<f32> delta(spec.cells());
    for (i32 iter = 0; iter < params.iterations; ++iter) {
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
                for (const Neighbour& nb : kNeighbours) {
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

} // namespace render::terraingen
