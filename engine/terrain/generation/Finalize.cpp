#include "engine/terrain/generation/Finalize.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/ThermalErosion.hpp"

namespace render::terraingen {

namespace {

constexpr u32 kSaltRelief = 0xf1e2d3c4u;
constexpr u32 kSaltStrata = 0x57a7a5b1u;

// Periodic altitude band in [0,1] with a hard ledge and a soft slope —
// the strata profile shared by the rockExposure mask and the geometric
// knob. `warp` is a world-anchored noise phase so contour lines bend.
f32 strataBand01(f32 h, f32 warp, f32 period) {
    const f32 t = (h + warp) / glm::max(period, 0.5f);
    const f32 frac = t - std::floor(t);
    return noise::smoothstep01(0.0f, 0.45f, frac) *
           (1.0f - noise::smoothstep01(0.7f, 0.95f, frac));
}

using terrain::catmullRom;

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
    chamferSweep(d, n, n);
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
                               const FinalizeParams& params, u32 seed,
                               const vector<f32>* fineScale,
                               const vector<f32>* deposit) {
    // World-coordinate sampler into the hydrology window grids.
    const auto hydroAt = [&](const vector<f32>& grid, f32 wx,
                             f32 wz) -> f32 {
        return bilinearWorld(hydroSpec, grid, wx, wz);
    };
    FinalizeResult out;
    const u32 f = glm::max(params.upsampleFactor, 1u);
    const f32 fineTexel = coarse.texelSize / static_cast<f32>(f);
    const f32 coarseSpan =
        static_cast<f32>(coarse.n - 1) * coarse.texelSize;
    if (params.fineSpan > 0.0f) {
        // Window-restricted fine grid (the bake path), clamped inside
        // the coarse rect and snapped to coarse texels.
        const f32 minX = glm::clamp(params.fineMinX, coarse.originX,
                                    coarse.originX + coarseSpan);
        const f32 minZ = glm::clamp(params.fineMinZ, coarse.originZ,
                                    coarse.originZ + coarseSpan);
        const f32 span = glm::min(
            params.fineSpan,
            glm::min(coarse.originX + coarseSpan - minX,
                     coarse.originZ + coarseSpan - minZ));
        out.fineSpec = GridSpec {
            minX, minZ, fineTexel,
            static_cast<u32>(std::lround(span / fineTexel)) + 1
        };
    } else {
        out.fineSpec = GridSpec { coarse.originX, coarse.originZ,
                                  fineTexel, (coarse.n - 1) * f + 1 };
    }
    // Fractional coarse-texel coords of a world point (the fine window
    // is no longer origin-aligned with the coarse grid).
    const auto coarseU = [&](f32 w, f32 origin) {
        return (w - origin) / coarse.texelSize;
    };

    // Coarse water cells (sea, lakes, river courses) and the chamfer
    // distance to them: the fine-erosion protection, the relief damp and
    // the S6 masks all read this field.
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

    // Coarse slope magnitude (central differences): the rock-exposure
    // mask and the strata gate both read it.
    vector<f32> slopeCoarse(coarse.cells());
    {
        const i32 cn = static_cast<i32>(coarse.n);
        for (i32 cz = 0; cz < cn; ++cz) {
            for (i32 cx = 0; cx < cn; ++cx) {
                const auto at = [&](i32 px, i32 pz) {
                    px = glm::clamp(px, 0, cn - 1);
                    pz = glm::clamp(pz, 0, cn - 1);
                    return eroded[static_cast<size_t>(pz) * coarse.n +
                                  static_cast<size_t>(px)];
                };
                const f32 gx = (at(cx + 1, cz) - at(cx - 1, cz)) /
                               (2.0f * coarse.texelSize);
                const f32 gz = (at(cx, cz + 1) - at(cx, cz - 1)) /
                               (2.0f * coarse.texelSize);
                slopeCoarse[static_cast<size_t>(cz) * coarse.n + cx] =
                    std::sqrt(gx * gx + gz * gz);
            }
        }
    }

    // S5a — upsample, with the fine-erosion amplification between the
    // macro texels when enabled: coarse -> mid grid (half the upsample),
    // carve ravines there, mid -> fine, then a short thermal pass breaks
    // the ravine walls into talus facets. Disabled, it is the plain
    // one-step bicubic of before.
    const bool amplifying =
        params.fine.iterations > 0 && params.fine.k > 0.0f;
    GridSpec midSpec;
    vector<f32> incision; // midSpec-sized when amplifying
    out.height.resize(out.fineSpec.cells());
    if (amplifying) {
        const u32 ratio = f >= 2 ? 2u : 1u; // fine texels per mid texel
        midSpec = GridSpec { out.fineSpec.originX, out.fineSpec.originZ,
                             out.fineSpec.texelSize *
                                 static_cast<f32>(ratio),
                             (out.fineSpec.n - 1) / ratio + 1 };
        vector<f32> mid(midSpec.cells());
        vector<f32> allow(midSpec.cells());
        vector<f32> discharge(midSpec.cells());
        vector<f32> scale;
        if (fineScale) {
            scale.resize(midSpec.cells());
        }
        const f32 hydroCellArea = hydroSpec.texelSize * hydroSpec.texelSize;
        for (u32 row = 0; row < midSpec.n; ++row) {
            for (u32 col = 0; col < midSpec.n; ++col) {
                const size_t i = static_cast<size_t>(row) * midSpec.n +
                                 col;
                const f32 x = midSpec.x(col);
                const f32 z = midSpec.z(row);
                const f32 u = coarseU(x, coarse.originX);
                const f32 v = coarseU(z, coarse.originZ);
                mid[i] = sampleGrid(coarse, eroded, u, v);
                // Protection: no carving through river corridors, lake
                // shores, beaches or the sea floor.
                const f32 dWater = bilinearGrid(coarse, waterDist, u, v);
                const f32 dSea =
                    bilinearGrid(coarse, macro.seaDist, u, v);
                allow[i] =
                    noise::smoothstep01(8.0f, 40.0f, dWater) *
                    noise::smoothstep01(params.beachBand * 0.5f,
                                        params.beachBand,
                                        glm::max(dSea, 0.0f));
                // Coarse discharge, log-normalized like the S6 flow
                // mask: the shared field that makes ravines converge
                // toward the macro drainage on both sides of a border.
                discharge[i] = glm::clamp(
                    std::log10(glm::max(hydroAt(hydro.area, x, z) /
                                            hydroCellArea,
                                        1.0f)) /
                        params.flowLogSpan,
                    0.0f, 1.0f);
                if (fineScale) {
                    scale[i] = bilinearGrid(coarse, *fineScale, u, v);
                }
            }
        }
        FineErosionResult amplified =
            amplifyFine(midSpec, mid, allow, discharge, params.fine,
                        fineScale ? &scale : nullptr);
        incision = std::move(amplified.incision);
        if (ratio == 1) {
            out.height = std::move(amplified.height);
        } else {
            for (u32 row = 0; row < out.fineSpec.n; ++row) {
                for (u32 col = 0; col < out.fineSpec.n; ++col) {
                    const f32 mu = static_cast<f32>(col) /
                                   static_cast<f32>(ratio);
                    const f32 mv = static_cast<f32>(row) /
                                   static_cast<f32>(ratio);
                    out.height[static_cast<size_t>(row) *
                                   out.fineSpec.n +
                               col] =
                        sampleGrid(midSpec, amplified.height, mu, mv);
                }
            }
        }
        if (params.fine.thermalIterations > 0) {
            ThermalParams thermal;
            thermal.iterations = params.fine.thermalIterations;
            thermal.talusTan = params.fine.thermalTalusTan;
            thermal.seaLevel = params.seaLevel;
            ThermalResult relaxed =
                erodeThermal(out.fineSpec, out.height, thermal);
            out.height = std::move(relaxed.height);
        }
    } else {
        for (u32 row = 0; row < out.fineSpec.n; ++row) {
            for (u32 col = 0; col < out.fineSpec.n; ++col) {
                const f32 u =
                    coarseU(out.fineSpec.x(col), coarse.originX);
                const f32 v =
                    coarseU(out.fineSpec.z(row), coarse.originZ);
                out.height[static_cast<size_t>(row) * out.fineSpec.n +
                           col] = sampleGrid(coarse, eroded, u, v);
            }
        }
    }
    // Bilinear incision sample (m) at world coords; 0 outside the fine
    // window or when amplification is off.
    const auto incisionAt = [&](f32 wx, f32 wz) -> f32 {
        if (incision.empty()) {
            return 0.0f;
        }
        const f32 span =
            static_cast<f32>(midSpec.n - 1) * midSpec.texelSize;
        if (wx < midSpec.originX || wz < midSpec.originZ ||
            wx > midSpec.originX + span || wz > midSpec.originZ + span) {
            return 0.0f;
        }
        return bilinearGrid(midSpec, incision,
                            (wx - midSpec.originX) / midSpec.texelSize,
                            (wz - midSpec.originZ) / midSpec.texelSize);
    };

    // S5b — mid-frequency relief, damped near/under water so shores,
    // lakes and river corridors stay clean, and inside fresh ravines so
    // the carve reads instead of fighting the noise.
    for (u32 row = 0; row < out.fineSpec.n; ++row) {
        for (u32 col = 0; col < out.fineSpec.n; ++col) {
            const f32 x = out.fineSpec.x(col);
            const f32 z = out.fineSpec.z(row);
            const f32 u = coarseU(x, coarse.originX);
            const f32 v = coarseU(z, coarse.originZ);
            const f32 dWater = bilinearGrid(coarse, waterDist, u, v);
            f32 damp =
                noise::smoothstep01(4.0f, params.wetnessReach, dWater);
            if (amplifying) {
                damp *= 1.0f - 0.6f * glm::clamp(incisionAt(x, z) /
                                                     params.fine.maxDepth,
                                                 0.0f, 1.0f);
            }
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

    // Optional geometric strata: ledge displacement on very steep faces
    // (shipped off — see the param note about the S1 terracing).
    if (params.strataAmplitude > 0.0f) {
        for (u32 row = 0; row < out.fineSpec.n; ++row) {
            for (u32 col = 0; col < out.fineSpec.n; ++col) {
                const f32 x = out.fineSpec.x(col);
                const f32 z = out.fineSpec.z(row);
                const f32 u = coarseU(x, coarse.originX);
                const f32 v = coarseU(z, coarse.originZ);
                const f32 gate = noise::smoothstep01(
                    0.7f, 1.0f, bilinearGrid(coarse, slopeCoarse, u, v));
                if (gate <= 0.0f) {
                    continue;
                }
                const f32 warp =
                    (noise::fbm(seed ^ kSaltStrata, x, z, 1.0f / 80.0f,
                                2, 2.0f, 0.5f) *
                         2.0f -
                     1.0f) *
                    4.0f;
                const f32 band = strataBand01(
                    sampleGrid(coarse, eroded, u, v), warp,
                    params.strataPeriod);
                out.height[static_cast<size_t>(row) * out.fineSpec.n +
                           col] +=
                    params.strataAmplitude * gate * (band - 0.5f);
            }
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

    // S5d-bis — lake-bed profile: every masked natural lake gets a
    // basin carved under its surface, deeper away from the shore (the
    // erosion+deposition floor often sits just under the spill — a
    // sheet with no water under it). Carve-only.
    if (params.lakeDepthCoef > 0.0f) {
        for (const Lake& lake : hydro.lakes) {
            if (lake.dug || lake.mask.empty() || lake.maskWidth < 3) {
                continue;
            }
            // Chamfer distance to the nearest DRY mask cell, in mask
            // texels (small grids: the giant basins are ~250^2).
            const i32 mw = static_cast<i32>(lake.maskWidth);
            const i32 mh = static_cast<i32>(lake.maskHeight);
            vector<f32> shore(static_cast<size_t>(mw) * mh, 1.0e30f);
            for (i32 row = 0; row < mh; ++row) {
                for (i32 col = 0; col < mw; ++col) {
                    const size_t i = static_cast<size_t>(row) * mw + col;
                    // Border cells and dry cells are the shore.
                    if (!lake.mask[i] || row == 0 || col == 0 ||
                        row == mh - 1 || col == mw - 1) {
                        shore[i] = 0.0f;
                    }
                }
            }
            chamferSweep(shore, mw, mh);
            // Carve the fine texels covered by wet mask cells.
            const i32 c0 = static_cast<i32>(
                std::floor((lake.minX - out.fineSpec.originX) /
                           out.fineSpec.texelSize));
            const i32 c1 = static_cast<i32>(
                std::ceil((lake.maxX - out.fineSpec.originX) /
                          out.fineSpec.texelSize));
            const i32 r0 = static_cast<i32>(
                std::floor((lake.minZ - out.fineSpec.originZ) /
                           out.fineSpec.texelSize));
            const i32 r1 = static_cast<i32>(
                std::ceil((lake.maxZ - out.fineSpec.originZ) /
                          out.fineSpec.texelSize));
            const i32 fineN = static_cast<i32>(out.fineSpec.n);
            for (i32 row = glm::max(r0, 0);
                 row <= glm::min(r1, fineN - 1); ++row) {
                for (i32 col = glm::max(c0, 0);
                     col <= glm::min(c1, fineN - 1); ++col) {
                    const f32 x = out.fineSpec.x(static_cast<u32>(col));
                    const f32 z = out.fineSpec.z(static_cast<u32>(row));
                    const f32 mu = (x - lake.minX) / lake.maskTexel;
                    const f32 mv = (z - lake.minZ) / lake.maskTexel;
                    const i32 mx = glm::clamp(
                        static_cast<i32>(std::floor(mu)), 0, mw - 2);
                    const i32 mz = glm::clamp(
                        static_cast<i32>(std::floor(mv)), 0, mh - 2);
                    const auto at = [&](i32 cx, i32 cz) {
                        return shore[static_cast<size_t>(cz) * mw + cx];
                    };
                    const f32 tu = glm::clamp(
                        mu - static_cast<f32>(mx), 0.0f, 1.0f);
                    const f32 tv = glm::clamp(
                        mv - static_cast<f32>(mz), 0.0f, 1.0f);
                    const f32 a = at(mx, mz) +
                                  (at(mx + 1, mz) - at(mx, mz)) * tu;
                    const f32 b =
                        at(mx, mz + 1) +
                        (at(mx + 1, mz + 1) - at(mx, mz + 1)) * tu;
                    const f32 shoreDist =
                        (a + (b - a) * tv) * lake.maskTexel;
                    if (shoreDist <= 0.0f || shoreDist > 1.0e29f) {
                        continue;
                    }
                    const f32 depth =
                        glm::min(params.lakeDepthCoef * shoreDist,
                                 params.lakeDepthMax);
                    const size_t i =
                        static_cast<size_t>(row) * out.fineSpec.n +
                        static_cast<size_t>(col);
                    out.height[i] =
                        glm::min(out.height[i], lake.level - depth);
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
    out.rockExposure.resize(coarse.cells());
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
            // Fresh ravines read damp and quiet: wetness up, detail
            // noise down where the fine erosion actually carved.
            const f32 incNorm = glm::clamp(
                incisionAt(coarse.x(col), coarse.z(row)) /
                    params.fine.maxDepth,
                0.0f, 1.0f);
            const f32 wet = glm::max(glm::max(flowNorm, nearWater),
                                     0.35f * incNorm);
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
                noise::smoothstep01(2.0f, 10.0f, waterDist[i]) *
                (1.0f - 0.6f * incNorm);
            out.flow[i] = toU8(flowNorm);
            out.wetness[i] = toU8(wet);
            out.beach[i] = toU8(beach);
            out.detailAmp[i] = toU8(detail);
            // Bare rock: steep eroded faces, minus scree aprons (the
            // deposit stays soil-covered), strata-banded so cliff
            // pixels read as geology instead of one flat material.
            const f32 scree =
                deposit ? noise::smoothstep01(0.3f, 1.5f, (*deposit)[i])
                        : 0.0f;
            const f32 warp =
                (noise::fbm(seed ^ kSaltStrata, coarse.x(col),
                            coarse.z(row), 1.0f / 80.0f, 2, 2.0f,
                            0.5f) *
                     2.0f -
                 1.0f) *
                4.0f;
            const f32 band =
                strataBand01(eroded[i], warp, params.strataPeriod);
            const f32 rock =
                noise::smoothstep01(0.35f, 0.75f, slopeCoarse[i]) *
                (1.0f - scree) * (0.7f + 0.3f * band);
            out.rockExposure[i] = toU8(rock);
        }
    }
    return out;
}

} // namespace render::terraingen
