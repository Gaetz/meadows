#include "engine/terrain/generation/FineErosion.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

namespace render::terraingen {

namespace {

} // namespace

FineErosionResult amplifyFine(const GridSpec& spec,
                              const vector<f32>& height,
                              const vector<f32>& allow,
                              const vector<f32>& discharge,
                              const FineErosionParams& params,
                              const vector<f32>* scale) {
    FineErosionResult out;
    out.height = height;
    out.incision.assign(spec.cells(), 0.0f);
    if (params.iterations <= 0 || params.k <= 0.0f) {
        return out;
    }
    const i32 n = static_cast<i32>(spec.n);
    const size_t cells = spec.cells();
    const f32 cellArea = spec.texelSize * spec.texelSize;

    vector<u32> receiver(cells);
    vector<f32> recvDist(cells);
    vector<f32> area(cells);
    vector<f32> gathered(cells);
    vector<f32> budget(cells, params.maxDepth);

    const i32 interval = glm::max(params.routingInterval, 1);
    for (i32 iter = 0; iter < params.iterations; ++iter) {
        if (iter % interval == 0) {
            // Raw steepest-descent receivers on the CURRENT surface. No
            // depression routing: a pit keeps itself and never carves.
            for (i32 cz = 0; cz < n; ++cz) {
                for (i32 cx = 0; cx < n; ++cx) {
                    const size_t i =
                        static_cast<size_t>(cz) * spec.n + cx;
                    receiver[i] = static_cast<u32>(i);
                    recvDist[i] = spec.texelSize;
                    f32 best = 0.0f;
                    for (const Neighbour& nb : kNeighbours8) {
                        const i32 px = cx + nb.dx;
                        const i32 pz = cz + nb.dz;
                        if (px < 0 || pz < 0 || px >= n || pz >= n) {
                            continue;
                        }
                        const size_t p =
                            static_cast<size_t>(pz) * spec.n + px;
                        const f32 dist = nb.dist * spec.texelSize;
                        const f32 slope =
                            (out.height[i] - out.height[p]) / dist;
                        if (slope > best) {
                            best = slope;
                            receiver[i] = static_cast<u32>(p);
                            recvDist[i] = dist;
                        }
                    }
                }
            }
            // Bounded accumulation: each gather sweep moves every
            // cell's area one receiver hop further, so reachSteps caps
            // the dependence radius at reachSteps * texelSize meters.
            std::fill(area.begin(), area.end(), cellArea);
            for (i32 step = 0; step < params.reachSteps; ++step) {
                std::fill(gathered.begin(), gathered.end(), 0.0f);
                for (size_t i = 0; i < cells; ++i) {
                    const u32 r = receiver[i];
                    if (r != i) {
                        gathered[r] += area[i];
                    }
                }
                bool grew = false;
                for (size_t i = 0; i < cells; ++i) {
                    const f32 next = cellArea + gathered[i];
                    if (next > area[i]) {
                        area[i] = next;
                        grew = true;
                    }
                }
                if (!grew) {
                    break;
                }
            }
        }
        // Explicit incision, fixed row-major order. The per-step clamp
        // to a fraction of the local drop keeps the scheme stable (no
        // slope inversion), the budget keeps the total honest.
        for (size_t i = 0; i < cells; ++i) {
            const u32 r = receiver[i];
            if (r == i || budget[i] <= 0.0f) {
                continue;
            }
            const f32 drop = out.height[i] - out.height[r];
            if (drop <= 0.0f) {
                continue;
            }
            const f32 slope = drop / recvDist[i];
            const f32 ks = scale ? (*scale)[i] : 1.0f;
            f32 dh = params.dt * params.k * ks *
                     std::pow(glm::min(area[i], params.areaCap),
                              params.areaExponent) *
                     slope * (1.0f + params.dischargeGain * discharge[i]) *
                     allow[i];
            dh = glm::min(dh, 0.35f * drop);
            dh = glm::min(dh, budget[i]);
            out.height[i] -= dh;
            budget[i] -= dh;
            out.incision[i] += dh;
        }
    }
    return out;
}

} // namespace render::terraingen
