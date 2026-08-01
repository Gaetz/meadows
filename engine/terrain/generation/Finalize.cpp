#include "engine/terrain/generation/Finalize.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/Noise.hpp"

namespace render::terraingen {

namespace {

constexpr u32 kSaltRelief = 0xf1e2d3c4u;

f32 catmullRom(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) {
    return p1 +
           0.5f * t *
               (p2 - p0 +
                t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                     t * (3.0f * (p1 - p2) + p3 - p0)));
}

// Bicubic sample of a coarse grid at fractional texel coords, clamped.
f32 sampleGrid(const GridSpec& spec, const vector<f32>& grid, f32 u,
               f32 v) {
    const f32 fu = std::floor(u);
    const f32 fv = std::floor(v);
    const i32 iu = static_cast<i32>(fu);
    const i32 iv = static_cast<i32>(fv);
    const f32 tu = u - fu;
    const f32 tv = v - fv;
    const auto at = [&](i32 cx, i32 cz) {
        cx = glm::clamp(cx, 0, static_cast<i32>(spec.n) - 1);
        cz = glm::clamp(cz, 0, static_cast<i32>(spec.n) - 1);
        return grid[static_cast<size_t>(cz) * spec.n +
                    static_cast<size_t>(cx)];
    };
    f32 rows[4];
    for (i32 j = 0; j < 4; ++j) {
        const i32 cz = iv - 1 + j;
        rows[j] = catmullRom(at(iu - 1, cz), at(iu, cz), at(iu + 1, cz),
                             at(iu + 2, cz), tu);
    }
    return catmullRom(rows[0], rows[1], rows[2], rows[3], tv);
}

f32 bilinearGrid(const GridSpec& spec, const vector<f32>& grid, f32 u,
                 f32 v) {
    const f32 cu = glm::clamp(u, 0.0f, static_cast<f32>(spec.n - 1));
    const f32 cv = glm::clamp(v, 0.0f, static_cast<f32>(spec.n - 1));
    const u32 u0 = glm::min(static_cast<u32>(cu), spec.n - 2);
    const u32 v0 = glm::min(static_cast<u32>(cv), spec.n - 2);
    const f32 tu = cu - static_cast<f32>(u0);
    const f32 tv = cv - static_cast<f32>(v0);
    const auto at = [&](u32 cx, u32 cz) {
        return grid[static_cast<size_t>(cz) * spec.n + cx];
    };
    const f32 a = at(u0, v0) + (at(u0 + 1, v0) - at(u0, v0)) * tu;
    const f32 b =
        at(u0, v0 + 1) + (at(u0 + 1, v0 + 1) - at(u0, v0 + 1)) * tu;
    return a + (b - a) * tv;
}

// Chamfer distance (meters) to the nearest set cell of `mask`.
vector<f32> distanceToMask(const GridSpec& spec, const vector<u8>& mask) {
    const i32 n = static_cast<i32>(spec.n);
    constexpr f32 kFar = 1.0e30f;
    vector<f32> d(spec.cells(), kFar);
    for (size_t i = 0; i < d.size(); ++i) {
        if (mask[i]) {
            d[i] = 0.0f;
        }
    }
    const auto idx = [n](i32 cx, i32 cz) {
        return static_cast<size_t>(cz) * static_cast<size_t>(n) + cx;
    };
    const auto relax = [&](i32 cx, i32 cz, i32 ox, i32 oz, f32 w) {
        const i32 px = cx + ox;
        const i32 pz = cz + oz;
        if (px < 0 || pz < 0 || px >= n || pz >= n) {
            return;
        }
        d[idx(cx, cz)] = glm::min(d[idx(cx, cz)], d[idx(px, pz)] + w);
    };
    for (i32 cz = 0; cz < n; ++cz) {
        for (i32 cx = 0; cx < n; ++cx) {
            relax(cx, cz, -1, 0, 1.0f);
            relax(cx, cz, 0, -1, 1.0f);
            relax(cx, cz, -1, -1, 1.41421356f);
            relax(cx, cz, 1, -1, 1.41421356f);
        }
    }
    for (i32 cz = n - 1; cz >= 0; --cz) {
        for (i32 cx = n - 1; cx >= 0; --cx) {
            relax(cx, cz, 1, 0, 1.0f);
            relax(cx, cz, 0, 1, 1.0f);
            relax(cx, cz, 1, 1, 1.41421356f);
            relax(cx, cz, -1, 1, 1.41421356f);
        }
    }
    for (f32& v : d) {
        v = v >= kFar ? kFar : v * spec.texelSize;
    }
    return d;
}

u8 toU8(f32 v01) {
    return static_cast<u8>(
        glm::clamp(v01 * 255.0f + 0.5f, 0.0f, 255.0f));
}

} // namespace

FinalizeResult finalizeTerrain(const GridSpec& coarse,
                               const vector<f32>& eroded,
                               const MacroResult& macro,
                               const HydrologyResult& hydro,
                               const GridSpec& hydroSpec,
                               const FinalizeParams& params, u32 seed) {
    // World-coordinate sampler into the hydrology window grids.
    const auto hydroAt = [&](const vector<f32>& grid, f32 wx,
                             f32 wz) -> f32 {
        const f32 u = glm::clamp((wx - hydroSpec.originX) /
                                     hydroSpec.texelSize,
                                 0.0f,
                                 static_cast<f32>(hydroSpec.n - 1));
        const f32 v = glm::clamp((wz - hydroSpec.originZ) /
                                     hydroSpec.texelSize,
                                 0.0f,
                                 static_cast<f32>(hydroSpec.n - 1));
        const u32 u0 = glm::min(static_cast<u32>(u), hydroSpec.n - 2);
        const u32 v0 = glm::min(static_cast<u32>(v), hydroSpec.n - 2);
        const f32 tu = u - static_cast<f32>(u0);
        const f32 tv = v - static_cast<f32>(v0);
        const auto at = [&](u32 cx, u32 cz) {
            return grid[static_cast<size_t>(cz) * hydroSpec.n + cx];
        };
        const f32 a = at(u0, v0) + (at(u0 + 1, v0) - at(u0, v0)) * tu;
        const f32 b = at(u0, v0 + 1) +
                      (at(u0 + 1, v0 + 1) - at(u0, v0 + 1)) * tu;
        return a + (b - a) * tv;
    };
    FinalizeResult out;
    const u32 f = glm::max(params.upsampleFactor, 1u);
    out.fineSpec = GridSpec { coarse.originX, coarse.originZ,
                              coarse.texelSize / static_cast<f32>(f),
                              (coarse.n - 1) * f + 1 };

    // S5a — bicubic upsample.
    out.height.resize(out.fineSpec.cells());
    for (u32 row = 0; row < out.fineSpec.n; ++row) {
        for (u32 col = 0; col < out.fineSpec.n; ++col) {
            const f32 u = static_cast<f32>(col) / static_cast<f32>(f);
            const f32 v = static_cast<f32>(row) / static_cast<f32>(f);
            out.height[static_cast<size_t>(row) * out.fineSpec.n + col] =
                sampleGrid(coarse, eroded, u, v);
        }
    }

    // S5b — mid-frequency relief, damped near/under water so shores,
    // lakes and river corridors stay clean. Uses the coarse water
    // distance (computed below) — order: masks first need water cells.
    vector<u8> waterMask(coarse.cells(), 0);
    for (u32 row = 0; row < coarse.n; ++row) {
        for (u32 col = 0; col < coarse.n; ++col) {
            const size_t i = static_cast<size_t>(row) * coarse.n + col;
            if (eroded[i] <= params.seaLevel ||
                hydroAt(hydro.lakeDepth, coarse.x(col), coarse.z(row)) >
                    0.0f) {
                waterMask[i] = 1;
            }
        }
    }
    for (const River& river : hydro.rivers) {
        for (const RiverPoint& pt : river.points) {
            const i32 cx = static_cast<i32>(
                std::lround((pt.x - coarse.originX) / coarse.texelSize));
            const i32 cz = static_cast<i32>(
                std::lround((pt.z - coarse.originZ) / coarse.texelSize));
            if (cx >= 0 && cz >= 0 && cx < static_cast<i32>(coarse.n) &&
                cz < static_cast<i32>(coarse.n)) {
                waterMask[static_cast<size_t>(cz) * coarse.n + cx] = 1;
            }
        }
    }
    const vector<f32> waterDist = distanceToMask(coarse, waterMask);
    for (u32 row = 0; row < out.fineSpec.n; ++row) {
        for (u32 col = 0; col < out.fineSpec.n; ++col) {
            const f32 x = out.fineSpec.x(col);
            const f32 z = out.fineSpec.z(row);
            const f32 u = static_cast<f32>(col) / static_cast<f32>(f);
            const f32 v = static_cast<f32>(row) / static_cast<f32>(f);
            const f32 dWater = bilinearGrid(coarse, waterDist, u, v);
            const f32 damp =
                noise::smoothstep01(4.0f, params.wetnessReach, dWater);
            if (damp <= 0.0f) {
                continue;
            }
            const f32 relief =
                (noise::fbm(seed ^ kSaltRelief, x, z,
                            1.0f / params.reliefWavelength, 3, 2.0f,
                            0.5f) *
                     2.0f -
                 1.0f) *
                params.reliefAmplitude * damp;
            out.height[static_cast<size_t>(row) * out.fineSpec.n + col] +=
                relief;
        }
    }

    // S5c — river carving on the fine grid: parabolic bed under the
    // water surface, bank shoulder blending back into the hillside.
    for (const River& river : hydro.rivers) {
        for (size_t s = 0; s + 1 < river.points.size(); ++s) {
            const RiverPoint& a = river.points[s];
            const RiverPoint& b = river.points[s + 1];
            const f32 maxHalf = glm::max(a.halfWidth, b.halfWidth);
            const f32 reach = maxHalf * (1.0f + params.bankShoulder) +
                              out.fineSpec.texelSize;
            const f32 minX = glm::min(a.x, b.x) - reach;
            const f32 maxX = glm::max(a.x, b.x) + reach;
            const f32 minZ = glm::min(a.z, b.z) - reach;
            const f32 maxZ = glm::max(a.z, b.z) + reach;
            const i32 c0 = static_cast<i32>(
                std::floor((minX - out.fineSpec.originX) /
                           out.fineSpec.texelSize));
            const i32 c1 = static_cast<i32>(
                std::ceil((maxX - out.fineSpec.originX) /
                          out.fineSpec.texelSize));
            const i32 r0 = static_cast<i32>(
                std::floor((minZ - out.fineSpec.originZ) /
                           out.fineSpec.texelSize));
            const i32 r1 = static_cast<i32>(
                std::ceil((maxZ - out.fineSpec.originZ) /
                          out.fineSpec.texelSize));
            const f32 abx = b.x - a.x;
            const f32 abz = b.z - a.z;
            const f32 abLen2 = abx * abx + abz * abz;
            for (i32 row = glm::max(r0, 0);
                 row <= glm::min(r1, static_cast<i32>(out.fineSpec.n) - 1);
                 ++row) {
                for (i32 col = glm::max(c0, 0);
                     col <=
                     glm::min(c1, static_cast<i32>(out.fineSpec.n) - 1);
                     ++col) {
                    const f32 x = out.fineSpec.x(static_cast<u32>(col));
                    const f32 z = out.fineSpec.z(static_cast<u32>(row));
                    const f32 t =
                        abLen2 > 0.0f
                            ? glm::clamp(((x - a.x) * abx +
                                          (z - a.z) * abz) /
                                             abLen2,
                                         0.0f, 1.0f)
                            : 0.0f;
                    const f32 px = a.x + abx * t;
                    const f32 pz = a.z + abz * t;
                    const f32 dist = std::sqrt((x - px) * (x - px) +
                                               (z - pz) * (z - pz));
                    // Floor on the CARVE width (not the ribbon): a
                    // spring-tapered head still digs a walkable bed —
                    // sub-texel bands carved nothing and left the thin
                    // ribbon under the relief noise.
                    const f32 half = glm::max(
                        glm::mix(a.halfWidth, b.halfWidth, t), 1.6f);
                    const f32 surface = glm::mix(a.surface, b.surface, t);
                    const f32 depth = glm::clamp(
                        params.riverDepthCoef * 2.0f * half,
                        params.riverDepthMin, params.riverDepthMax);
                    const size_t i =
                        static_cast<size_t>(row) * out.fineSpec.n +
                        static_cast<size_t>(col);
                    const f32 n = dist / glm::max(half, 0.01f);
                    if (n <= 1.0f) {
                        // Parabolic bed up to the waterline at the bank.
                        const f32 bed = surface - depth * (1.0f - n * n);
                        out.height[i] = glm::min(out.height[i], bed);
                    } else if (n <= 1.0f + params.bankShoulder) {
                        // Bank shoulder: never below the waterline, and
                        // blending up into the untouched hillside.
                        const f32 blend =
                            noise::smoothstep01(1.0f,
                                                1.0f + params.bankShoulder,
                                                n);
                        const f32 cap =
                            glm::mix(surface, out.height[i], blend);
                        out.height[i] = glm::min(
                            out.height[i], glm::max(cap, surface));
                    }
                }
            }
        }
    }

    // S5d — dig the placed ponds (Lake::dug): a parabolic dish under the
    // pond level. Natural lakes already sit in their eroded basin.
    for (const Lake& pond : hydro.lakes) {
        if (!pond.dug) {
            continue;
        }
        const f32 cx = (pond.minX + pond.maxX) * 0.5f;
        const f32 cz = (pond.minZ + pond.maxZ) * 0.5f;
        const f32 radius = glm::max((pond.maxX - pond.minX) * 0.5f, 1.0f);
        const i32 c0 = static_cast<i32>(
            std::floor((pond.minX - out.fineSpec.originX) /
                       out.fineSpec.texelSize));
        const i32 c1 = static_cast<i32>(
            std::ceil((pond.maxX - out.fineSpec.originX) /
                      out.fineSpec.texelSize));
        const i32 r0 = static_cast<i32>(
            std::floor((pond.minZ - out.fineSpec.originZ) /
                       out.fineSpec.texelSize));
        const i32 r1 = static_cast<i32>(
            std::ceil((pond.maxZ - out.fineSpec.originZ) /
                      out.fineSpec.texelSize));
        for (i32 row = glm::max(r0, 0);
             row <= glm::min(r1, static_cast<i32>(out.fineSpec.n) - 1);
             ++row) {
            for (i32 col = glm::max(c0, 0);
                 col <=
                 glm::min(c1, static_cast<i32>(out.fineSpec.n) - 1);
                 ++col) {
                const f32 x = out.fineSpec.x(static_cast<u32>(col));
                const f32 z = out.fineSpec.z(static_cast<u32>(row));
                const f32 d =
                    std::sqrt((x - cx) * (x - cx) + (z - cz) * (z - cz)) /
                    radius;
                if (d > 1.0f) {
                    continue;
                }
                const f32 bed =
                    pond.level - params.riverDepthMax * 0.5f *
                                     glm::max(1.0f - d * d, 0.0f);
                const size_t i =
                    static_cast<size_t>(row) * out.fineSpec.n +
                    static_cast<size_t>(col);
                out.height[i] = glm::min(out.height[i], bed);
            }
        }
    }

    // S6 — coarse mask channels.
    const f32 cellArea = coarse.texelSize * coarse.texelSize;
    out.flow.resize(coarse.cells());
    out.wetness.resize(coarse.cells());
    out.beach.resize(coarse.cells());
    out.detailAmp.resize(coarse.cells());
    for (u32 row = 0; row < coarse.n; ++row) {
        for (u32 col = 0; col < coarse.n; ++col) {
            const size_t i = static_cast<size_t>(row) * coarse.n + col;
            const f32 flowNorm = glm::clamp(
                std::log10(glm::max(
                    hydroAt(hydro.area, coarse.x(col), coarse.z(row)) /
                        cellArea,
                    1.0f)) /
                    params.flowLogSpan,
                0.0f, 1.0f);
            const f32 nearWater =
                1.0f - noise::smoothstep01(0.0f, params.wetnessReach,
                                           waterDist[i]);
            const f32 wet = glm::max(flowNorm, nearWater);
            // Beach: the shore band, on land, minus underwater texels.
            const f32 d = macro.seaDist[i];
            const f32 beach =
                d >= 0.0f
                    ? 1.0f - noise::smoothstep01(params.beachBand * 0.5f,
                                                 params.beachBand, d)
                    : 0.0f;
            // Detail dies on beaches and under/near water, breathes on
            // dry land.
            const f32 detail =
                (1.0f - beach) *
                noise::smoothstep01(2.0f, 10.0f, waterDist[i]);
            out.flow[i] = toU8(flowNorm);
            out.wetness[i] = toU8(wet);
            out.beach[i] = toU8(beach);
            out.detailAmp[i] = toU8(detail);
        }
    }
    return out;
}

} // namespace render::terraingen
