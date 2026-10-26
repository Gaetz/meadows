#include "engine/terrain/generation/Finalize.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include "engine/reflect/Fields.hpp"

#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/ThermalErosion.hpp"

namespace render::terraingen {

namespace {

// The carve profile of a river tier: the moddable FinalizeParams columns
// (depth cap, bank shoulder) joined to the fixed TierTraits row.
struct TierProfile {
    f32 depthMax;
    f32 shoulder;
    f32 flatBoost;
};
TierProfile tierProfile(const FinalizeParams& p, u8 tier) {
    const TierTraits& traits = tierTraits(tier);
    switch (tier) {
    case 0: return { p.streamDepthMax, p.bankShoulder, traits.flatBoost };
    case 2: return { p.fleuveDepthMax, p.fleuveBankShoulder, traits.flatBoost };
    default: return { p.riverDepthMax, p.bankShoulder, traits.flatBoost };
    }
}

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

FinalizeResult finalizeGround(const GridSpec& coarse,
                              const vector<f32>& eroded,
                              const MacroResult& macro,
                              const vector<f32>* drainage,
                              const GridSpec* drainageSpec,
                              const FinalizeParams& params, u32 seed,
                              const vector<f32>* fineScale,
                              const std::atomic<bool>* cancel) {
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

    // SEA cells and the chamfer distance to them: the ground pass
    // knows no other water (two-phase finalize) — the relief damp
    // keeps the COASTS quiet; channels and basins belong to the water
    // pass's carves, on ground that is final by then.
    vector<u8> waterMask(coarse.cells(), 0);
    for (size_t i = 0; i < coarse.cells(); ++i) {
        if (eroded[i] <= params.seaLevel) {
            waterMask[i] = 1;
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
        const f32 hydroCellArea =
            drainageSpec
                ? drainageSpec->texelSize * drainageSpec->texelSize
                : coarse.texelSize * coarse.texelSize;
        for (u32 row = 0; row < midSpec.n; ++row) {
            for (u32 col = 0; col < midSpec.n; ++col) {
                const size_t i = static_cast<size_t>(row) * midSpec.n +
                                 col;
                const f32 x = midSpec.x(col);
                const f32 z = midSpec.z(row);
                const f32 u = coarseU(x, coarse.originX);
                const f32 v = coarseU(z, coarse.originZ);
                mid[i] = bicubicGrid(coarse, eroded, u, v);
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
                const f32 acc =
                    drainage && drainageSpec
                        ? bilinearWorld(*drainageSpec, *drainage, x, z)
                        : 0.0f;
                discharge[i] = glm::clamp(
                    std::log10(glm::max(acc / hydroCellArea, 1.0f)) /
                        params.flowLogSpan,
                    0.0f, 1.0f);
                if (fineScale) {
                    scale[i] = bilinearGrid(coarse, *fineScale, u, v);
                }
            }
        }
        FineErosionResult amplified =
            amplifyFine(midSpec, mid, allow, discharge, params.fine,
                        fineScale ? &scale : nullptr, cancel);
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
                        bicubicGrid(midSpec, amplified.height, mu, mv);
                }
            }
        }
        if (params.fine.thermalIterations > 0) {
            ThermalParams thermal;
            thermal.iterations = params.fine.thermalIterations;
            thermal.talusTan = params.fine.thermalTalusTan;
            thermal.seaLevel = params.seaLevel;
            // The walls the coarse pass kept (steeper than ~35 deg
            // on the coarse grid) hold through the micro-thermal
            // pass: a designed cliff is not a ravine wall to facet.
            vector<f32> coarseSlope(coarse.cells(), 0.0f);
            for (u32 row = 1; row + 1 < coarse.n; ++row) {
                for (u32 col = 1; col + 1 < coarse.n; ++col) {
                    const size_t i = static_cast<size_t>(row) * coarse.n + col;
                    const f32 gx = (eroded[i + 1] - eroded[i - 1]) /
                                   (2.0f * coarse.texelSize);
                    const f32 gz = (eroded[i + coarse.n] - eroded[i - coarse.n]) /
                                   (2.0f * coarse.texelSize);
                    coarseSlope[i] = std::sqrt(gx * gx + gz * gz);
                }
            }
            vector<f32> hold(out.fineSpec.cells());
            for (u32 row = 0; row < out.fineSpec.n; ++row) {
                for (u32 col = 0; col < out.fineSpec.n; ++col) {
                    const f32 u = coarseU(out.fineSpec.x(col), coarse.originX);
                    const f32 v = coarseU(out.fineSpec.z(row), coarse.originZ);
                    const f32 sl = bicubicGrid(coarse, coarseSlope, u, v);
                    hold[static_cast<size_t>(row) * out.fineSpec.n + col] =
                        1.0f + 2.0f * glm::smoothstep(0.6f, 1.0f, sl);
                }
            }
            ThermalResult relaxed = erodeThermal(
                out.fineSpec, out.height, thermal, &hold, cancel);
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
                           col] = bicubicGrid(coarse, eroded, u, v);
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
                    bicubicGrid(coarse, eroded, u, v), warp,
                    params.strataPeriod);
                out.height[static_cast<size_t>(row) * out.fineSpec.n +
                           col] +=
                    params.strataAmplitude * gate * (band - 0.5f);
            }
        }
    }

    // Ground pass ends here: export the incision norm for the water
    // pass's masks. The water carves live in finalizeWater.
    out.incisionNorm.assign(coarse.cells(), 0.0f);
    if (!incision.empty() && params.fine.maxDepth > 0.0f) {
        for (u32 row = 0; row < coarse.n; ++row) {
            for (u32 col = 0; col < coarse.n; ++col) {
                out.incisionNorm[static_cast<size_t>(row) * coarse.n +
                                 col] =
                    glm::clamp(incisionAt(coarse.x(col),
                                          coarse.z(row)) /
                                   params.fine.maxDepth,
                               0.0f, 1.0f);
            }
        }
    }
    return out;
}

namespace {

// The inputs every water-pass step samples: the ground-pass result being
// carved, the coarse ground, and the hydrology on its own window grid.
struct WaterPass {
    FinalizeResult& out;
    const GridSpec& coarse;
    const vector<f32>& eroded;
    const MacroResult& macro;
    const HydrologyResult& hydro;
    const GridSpec& hydroSpec;
    const FinalizeParams& params;
    u32 seed;
    const vector<f32>* deposit;
};

constexpr f32 kMouthSandReach = 250.0f; // hand-tuned
constexpr f32 kHeadWetReach = 24.0f;    // hand-tuned

// Water cells (sea, lakes, river courses) on the coarse grid and the
// chamfer distance to them — the mask channels read it.
vector<f32> waterDistance(const WaterPass& w) {
    const GridSpec& coarse = w.coarse;
    const HydrologyResult& hydro = w.hydro;
    vector<u8> waterMask(coarse.cells(), 0);
    for (u32 row = 0; row < coarse.n; ++row) {
        for (u32 col = 0; col < coarse.n; ++col) {
            const size_t i = static_cast<size_t>(row) * coarse.n + col;
            if (w.eroded[i] <= w.params.seaLevel ||
                bilinearWorld(w.hydroSpec, hydro.lakeDepth, coarse.x(col),
                              coarse.z(row)) > 0.0f) {
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
    // Masked lakes too: hydro.lakeDepth is the WINDOW flood, frozen
    // before the dug ponds and the discovered pools existed — their
    // shores got no wetness, no beach and full detail noise in the
    // bed (dry-looking ponds).
    for (const Lake& lake : hydro.lakes) {
        if (lake.mask.empty() || lake.maskWidth == 0) {
            continue;
        }
        for (u32 row = 0; row < coarse.n; ++row) {
            const f32 z = coarse.z(row);
            if (z < lake.minZ || z > lake.maxZ) {
                continue;
            }
            for (u32 col = 0; col < coarse.n; ++col) {
                const f32 x = coarse.x(col);
                if (lake.covers(x, z)) {
                    waterMask[static_cast<size_t>(row) * coarse.n +
                              col] = 1;
                }
            }
        }
    }
    return distanceToMask(coarse, waterMask);
}

// River carving on the fine grid: parabolic bed under the water
// surface, bank shoulder blending back into the hillside. `preCarve`
// is the ground before any carve: the carve REFERENCE ground must not
// read cells this very pass already dug (order-dependence).
void carveRiverBeds(WaterPass& w, const vector<f32>& preCarve) {
    FinalizeResult& out = w.out;
    const FinalizeParams& params = w.params;
    for (const River& river : w.hydro.rivers) {
        // Tier character: cap band + shoulder (the ruisseau's shallow
        // cap is what keeps it wadeable whatever its width says).
        const TierProfile tier = tierProfile(params, river.tier);
        const f32 tierDepthMax = tier.depthMax;
        const f32 tierDepthMin =
            glm::min(params.riverDepthMin, tierDepthMax);
        const f32 shoulder = tier.shoulder;
        for (size_t s = 0; s + 1 < river.points.size(); ++s) {
            const RiverPoint& a = river.points[s];
            const RiverPoint& b = river.points[s + 1];
            const f32 maxHalf = glm::max(a.halfWidth, b.halfWidth);
            const f32 reach = maxHalf * (1.0f + shoulder) +
                              out.fineSpec.texelSize;
            // Nearest ford to this segment (few per river): outside its
            // influence the whole segment skips the per-texel test.
            f32 fordX = 0.0f;
            f32 fordZ = 0.0f;
            f32 fordNear = 1.0e30f;
            const f32 fordFade = params.fordRadius * 2.0f;
            for (const Vec2& ford : river.fords) {
                const f32 mx = (a.x + b.x) * 0.5f - ford.x;
                const f32 mz = (a.z + b.z) * 0.5f - ford.y;
                const f32 d = mx * mx + mz * mz;
                if (d < fordNear) {
                    fordNear = d;
                    fordX = ford.x;
                    fordZ = ford.y;
                }
            }
            const bool fordHere =
                fordNear < (fordFade + 40.0f) * (fordFade + 40.0f);
            const f32 minX = glm::min(a.x, b.x) - reach;
            const f32 maxX = glm::max(a.x, b.x) + reach;
            const f32 minZ = glm::min(a.z, b.z) - reach;
            const f32 maxZ = glm::max(a.z, b.z) + reach;
            const auto [c0, c1, r0, r1] =
                out.fineSpec.cellRange(minX, minZ, maxX, maxZ);
            const f32 abx = b.x - a.x;
            const f32 abz = b.z - a.z;
            const f32 abLen2 = abx * abx + abz * abz;
            // Flat-reach deepening (dev request): on a near-level
            // bief the reconciled backwater profile rides the
            // highest bed bump, and small ribbons stood PROUD of the
            // plain beside their channel. Digging the flat reaches a
            // bit deeper settles the waterline below the banks;
            // sloped reaches are untouched, and the ruisseau keeps a
            // smaller boost so it stays wadeable.
            const f32 segLen = std::sqrt(abLen2);
            const f32 segSlope =
                segLen > 1.0f ? (a.surface - b.surface) / segLen
                              : 1.0f;
            const f32 flatBoost =
                tier.flatBoost *
                (1.0f -
                 noise::smoothstep01(0.01f, 0.03f, segSlope));
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
                    // Carve WATERLINE = the lower of the baked
                    // surface and the local PRE-CARVE ground: where
                    // the pre-finalize hydrology surface HANGS above
                    // the eroded terrain, a surface-referenced dig
                    // never reached the ground — no channel at all,
                    // and the reconciled water then stood meters
                    // PROUD on flat ground (measured dev,
                    // (9364, 1608): 2.6 m wall of water, zero carve).
                    const i32 pc = glm::clamp(
                        static_cast<i32>(std::lround(
                            (px - out.fineSpec.originX) /
                            out.fineSpec.texelSize)),
                        0, static_cast<i32>(out.fineSpec.n) - 1);
                    const i32 pr = glm::clamp(
                        static_cast<i32>(std::lround(
                            (pz - out.fineSpec.originZ) /
                            out.fineSpec.texelSize)),
                        0, static_cast<i32>(out.fineSpec.n) - 1);
                    const f32 centerGround =
                        preCarve[static_cast<size_t>(pr) *
                                     out.fineSpec.n +
                                 static_cast<size_t>(pc)];
                    // A course buried deeper under the local ground
                    // than its own carve envelope is a window-vs-fine
                    // wall disagreement, not a channel: digging there
                    // cut 150 m slot canyons one texel wide through
                    // the walls (razor cliffs, measured). Detail bumps
                    // to shave stay well inside the envelope.
                    if (glm::mix(a.surface, b.surface, t) <
                        centerGround -
                            (tierDepthMax + params.bankMaxCut + 4.0f)) {
                        continue;
                    }
                    const f32 surface = glm::min(
                        glm::mix(a.surface, b.surface, t),
                        centerGround + 0.3f);
                    f32 depth = glm::clamp(
                                    params.riverDepthCoef * 2.0f * half,
                                    tierDepthMin, tierDepthMax) +
                                flatBoost;
                    if (fordHere) {
                        // The ford: the bed rises to wading depth over
                        // the spot, blending back to the channel.
                        const f32 fd = std::sqrt(
                            (px - fordX) * (px - fordX) +
                            (pz - fordZ) * (pz - fordZ));
                        depth = glm::mix(
                            glm::min(depth, params.fordDepth), depth,
                            noise::smoothstep01(params.fordRadius,
                                                fordFade, fd));
                    }
                    const size_t i =
                        static_cast<size_t>(row) * out.fineSpec.n +
                        static_cast<size_t>(col);
                    const f32 n = dist / glm::max(half, 0.01f);
                    // The deepest this carve may go: a bank below its
                    // own WATERLINE. Anchoring the cut to the local
                    // ground instead left every detail bump above the
                    // surface un-shaved — dotted rivers (the surface
                    // now lives on the final ground, so a
                    // waterline-anchored cut is bounded by nature).
                    // floorCut bounds the DIG only — it must never
                    // RAISE the ground: where the window course hangs
                    // over a real fine valley, max(.., floorCut) used
                    // to lift the valley floor into a flat plug with
                    // one-texel razor edges (the pathological cliffs,
                    // measured up to 190 m).
                    const f32 floorCut =
                        surface - depth - params.bankMaxCut;
                    if (n <= 1.0f) {
                        // Parabolic bed up to the waterline at the bank.
                        const f32 bed = surface - depth * (1.0f - n * n);
                        out.height[i] = glm::min(
                            out.height[i],
                            glm::max(glm::min(out.height[i], bed),
                                     floorCut));
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
                            out.height[i],
                            glm::max(glm::min(out.height[i],
                                              glm::max(cap, surface)),
                                     floorCut));
                    }
                }
            }
        }
    }
}

// Lake-bed profile: every masked natural lake gets a basin carved under
// its surface, deeper away from the shore (the erosion+deposition floor
// often sits just under the spill — a sheet with no water under it).
// Carve-only.
void carveLakeBasins(WaterPass& w) {
    FinalizeResult& out = w.out;
    const FinalizeParams& params = w.params;
    if (!(params.lakeDepthCoef > 0.0f)) {
        return;
    }
    for (const Lake& lake : w.hydro.lakes) {
        // Both mask dims: the bilinear shore read below taps
        // (mx + 1, mz + 1) — a one-row mask indexed off its end.
        if (lake.dug || lake.mask.empty() || lake.maskWidth < 3 ||
            lake.maskHeight < 3) {
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
        const auto [c0, c1, r0, r1] =
            out.fineSpec.cellRange(lake.minX, lake.minZ, lake.maxX, lake.maxZ);
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
                // Only cells that can HOLD the sheet become bed:
                // the mask is window-resolution, and where the
                // fine ground stands above the level, cutting it
                // punched a one-texel wall along the mask edge
                // (razor cliffs, measured up to 276 m at pond
                // rims). The dry rim is shore, not bed.
                if (out.height[i] > lake.level + 0.5f) {
                    continue;
                }
                // Bounded by the SHEET: a bank below the level
                // at most (a ground-anchored bound left detail
                // bumps poking through the sheet). Dig-only: the
                // bound never raises a deeper pre-existing floor.
                out.height[i] = glm::min(
                    out.height[i],
                    glm::max(glm::min(out.height[i],
                                      lake.level - depth),
                             lake.level - depth -
                                 params.bankMaxCut));
            }
        }
    }
}

// Dig the placed ponds (Lake::dug): a parabolic dish under the pond
// level. Natural lakes already sit in their eroded basin.
void digPlacedPonds(WaterPass& w) {
    FinalizeResult& out = w.out;
    const FinalizeParams& params = w.params;
    for (const Lake& pond : w.hydro.lakes) {
        if (!pond.dug) {
            continue;
        }
        const f32 cx = (pond.minX + pond.maxX) * 0.5f;
        const f32 cz = (pond.minZ + pond.maxZ) * 0.5f;
        const f32 radius = glm::max((pond.maxX - pond.minX) * 0.5f, 1.0f);
        const auto [c0, c1, r0, r1] =
            out.fineSpec.cellRange(pond.minX, pond.minZ, pond.maxX, pond.maxZ);
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
                // Only cells that can HOLD the sheet become bed: the
                // junction disc is sized on ribbon widths, not on the
                // terrain, and where it overlaps a hillside above the
                // level the full-disc dig punched a crater with a
                // one-texel rim wall (59 ponds with walls over 6 m on
                // one map, worst 276 m — the dev's razor cliffs). The
                // dry part of the disc is land, not bed.
                if (out.height[i] > pond.level + 0.5f) {
                    continue;
                }
                // Bounded by the pond's own sheet: dish plus a
                // bank below the LEVEL at most. Dig-only — the bound
                // never raises a deeper pre-existing floor.
                const f32 floorCut = pond.level -
                                     params.riverDepthMax * 0.5f -
                                     params.bankMaxCut;
                out.height[i] = glm::min(
                    out.height[i],
                    glm::max(glm::min(out.height[i], bed), floorCut));
            }
        }
    }
}

// Sea mouths: the tail of every rivière/fleuve that reaches the sea.
// The beach widens into a sand fan around each one.
vector<Vec2> collectSeaMouths(const WaterPass& w) {
    vector<Vec2> mouths;
    for (const River& river : w.hydro.rivers) {
        if (river.tier < 1 || river.points.empty()) {
            continue;
        }
        const RiverPoint& tail = river.points.back();
        if (tail.surface <= w.params.seaLevel + 0.5f) {
            mouths.push_back({ tail.x, tail.z });
        }
    }
    return mouths;
}

// Course heads (true sources, not lake resumes): the ground around a
// spring reads damp — wetness stamped in a small fan.
vector<Vec2> collectSpringHeads(const WaterPass& w) {
    vector<Vec2> heads;
    for (const River& river : w.hydro.rivers) {
        if (river.lakeFed || river.points.empty()) {
            continue;
        }
        const RiverPoint& head = river.points.front();
        if (head.surface > w.params.seaLevel + 0.5f) {
            heads.push_back({ head.x, head.z });
        }
    }
    return heads;
}

// Radial falloff [0,1] from the nearest of `centers`: 1 at the center,
// 0 at `reach` and beyond.
f32 fanFalloff01(const vector<Vec2>& centers, f32 reach, f32 x, f32 z) {
    f32 best = 1.0e30f;
    for (const Vec2& c : centers) {
        const f32 dx = x - c.x;
        const f32 dz = z - c.y;
        best = glm::min(best, dx * dx + dz * dz);
    }
    if (best > reach * reach) {
        return 0.0f;
    }
    return 1.0f - noise::smoothstep01(0.0f, reach, std::sqrt(best));
}

// Coarse slope for the rock mask — computed on the FINAL ground
// resampled at the same 16 m lattice, AFTER the carves: the gorge
// walls, pierce notches and lake beds are carve products the
// stage-1 slope never saw, so they wore meadow materials draped
// on near-vertical rock. Same grid, same thresholds: the look
// only moves where the carves moved the ground. Outside this
// slice's fine rect the stage-1 value remains.
vector<f32> finalGroundSlope(const WaterPass& w) {
    const FinalizeResult& out = w.out;
    const GridSpec& coarse = w.coarse;
    vector<f32> slopeCoarse(coarse.cells());
    const i32 cn = static_cast<i32>(coarse.n);
    const f32 fineMaxX =
        out.fineSpec.originX +
        static_cast<f32>(out.fineSpec.n - 1) *
            out.fineSpec.texelSize;
    const f32 fineMaxZ =
        out.fineSpec.originZ +
        static_cast<f32>(out.fineSpec.n - 1) *
            out.fineSpec.texelSize;
    const auto finalAt = [&](i32 px, i32 pz) {
        px = glm::clamp(px, 0, cn - 1);
        pz = glm::clamp(pz, 0, cn - 1);
        const f32 wx = coarse.x(static_cast<u32>(px));
        const f32 wz = coarse.z(static_cast<u32>(pz));
        if (wx >= out.fineSpec.originX && wx <= fineMaxX &&
            wz >= out.fineSpec.originZ && wz <= fineMaxZ) {
            return bilinearWorld(out.fineSpec, out.height, wx, wz);
        }
        return w.eroded[static_cast<size_t>(pz) * coarse.n +
                        static_cast<size_t>(px)];
    };
    for (i32 cz = 0; cz < cn; ++cz) {
        for (i32 cx = 0; cx < cn; ++cx) {
            const f32 gx =
                (finalAt(cx + 1, cz) - finalAt(cx - 1, cz)) /
                (2.0f * coarse.texelSize);
            const f32 gz =
                (finalAt(cx, cz + 1) - finalAt(cx, cz - 1)) /
                (2.0f * coarse.texelSize);
            slopeCoarse[static_cast<size_t>(cz) * coarse.n + cx] =
                std::sqrt(gx * gx + gz * gz);
        }
    }
    return slopeCoarse;
}

// The coarse mask channels (flow, wetness, beach, detailAmp,
// rockExposure), read off the carved ground, the water distance and
// the pre-carve snapshot (a freshly cut face is bared rock).
void writeMaskChannels(WaterPass& w, const vector<f32>& waterDist,
                       const vector<f32>& preCarve,
                       const vector<Vec2>& mouths,
                       const vector<Vec2>& heads) {
    FinalizeResult& out = w.out;
    const GridSpec& coarse = w.coarse;
    const FinalizeParams& params = w.params;
    const f32 cellArea = coarse.texelSize * coarse.texelSize;
    out.flow.resize(coarse.cells());
    out.wetness.resize(coarse.cells());
    out.beach.resize(coarse.cells());
    out.detailAmp.resize(coarse.cells());
    out.rockExposure.resize(coarse.cells());
    const f32 fineMaxX =
        out.fineSpec.originX +
        static_cast<f32>(out.fineSpec.n - 1) * out.fineSpec.texelSize;
    const f32 fineMaxZ =
        out.fineSpec.originZ +
        static_cast<f32>(out.fineSpec.n - 1) * out.fineSpec.texelSize;
    const vector<f32> slopeCoarse = finalGroundSlope(w);
    for (u32 row = 0; row < coarse.n; ++row) {
        for (u32 col = 0; col < coarse.n; ++col) {
            const size_t i = static_cast<size_t>(row) * coarse.n + col;
            const f32 flowNorm = glm::clamp(
                std::log10(glm::max(
                    bilinearWorld(w.hydroSpec, w.hydro.area, coarse.x(col),
                                  coarse.z(row)) /
                        cellArea,
                    1.0f)) /
                    params.flowLogSpan,
                0.0f, 1.0f);
            const f32 nearWater =
                1.0f - noise::smoothstep01(0.0f, params.wetnessReach,
                                           waterDist[i]);
            // Fresh ravines read damp and quiet: wetness up, detail
            // noise down where the fine erosion actually carved.
            const f32 incNorm =
                out.incisionNorm.empty() ? 0.0f : out.incisionNorm[i];
            f32 wet = glm::max(glm::max(flowNorm, nearWater),
                               0.35f * incNorm);
            if (!heads.empty()) {
                wet = glm::max(
                    wet, fanFalloff01(heads, kHeadWetReach, coarse.x(col),
                                      coarse.z(row)));
            }
            // Beach: the shore band, on land, minus underwater texels.
            const f32 d = w.macro.seaDist[i];
            f32 beach =
                d >= 0.0f
                    ? 1.0f - noise::smoothstep01(params.beachBand * 0.5f,
                                                 params.beachBand, d)
                    : 0.0f;
            if (d >= -1.0f && !mouths.empty()) {
                beach = glm::max(
                    beach, fanFalloff01(mouths, kMouthSandReach,
                                        coarse.x(col), coarse.z(row)));
            }
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
                w.deposit ? noise::smoothstep01(0.3f, 1.5f, (*w.deposit)[i])
                          : 0.0f;
            const f32 warp =
                (noise::fbm(w.seed ^ kSaltStrata, coarse.x(col),
                            coarse.z(row), 1.0f / 80.0f, 2, 2.0f,
                            0.5f) *
                     2.0f -
                 1.0f) *
                4.0f;
            const f32 band =
                strataBand01(w.eroded[i], warp, params.strataPeriod);
            // A face the carve or the pierce CUT is freshly bared
            // rock whatever the deposit said: the scree term described
            // the pre-cut soil apron, and it kept every carved gorge
            // wall dressed as meadow. The cut term bypasses scree
            // only; the slope gate still keeps carved BEDS (flat)
            // rock-free.
            // The cut is sampled over the 16 m NEIGHBOURHOOD: a
            // gorge wall is not itself dug — it stands because the
            // bed BESIDE it was. The bed stays rock-free through the
            // slope gate (flat), the wall fires through its deep-cut
            // neighbour.
            f32 cut = 0.0f;
            for (i32 dn = 0; dn < 5; ++dn) {
                static constexpr f32 kOff[5][2] = { { 0.0f, 0.0f },
                                                    { 16.0f, 0.0f },
                                                    { -16.0f, 0.0f },
                                                    { 0.0f, 16.0f },
                                                    { 0.0f, -16.0f } };
                const f32 wx = coarse.x(col) + kOff[dn][0];
                const f32 wz = coarse.z(row) + kOff[dn][1];
                if (wx < out.fineSpec.originX || wx > fineMaxX ||
                    wz < out.fineSpec.originZ || wz > fineMaxZ) {
                    continue;
                }
                cut = glm::max(
                    cut,
                    bilinearWorld(out.fineSpec, preCarve, wx, wz) -
                        bilinearWorld(out.fineSpec, out.height, wx,
                                      wz));
            }
            // (The pierce gorges arrive already cut in preCarve;
            // their rim saddles carry little deposit, so the slope
            // gate alone bares them.)
            const f32 cutFresh =
                noise::smoothstep01(2.0f, 6.0f, cut);
            const f32 rock =
                noise::smoothstep01(0.35f, 0.75f, slopeCoarse[i]) *
                glm::max(1.0f - scree, cutFresh) *
                (0.7f + 0.3f * band);
            out.rockExposure[i] = toU8(rock);
        }
    }
}

} // namespace

void finalizeWater(FinalizeResult& out, const GridSpec& coarse,
                   const vector<f32>& eroded, const MacroResult& macro,
                   const HydrologyResult& hydro,
                   const GridSpec& hydroSpec,
                   const FinalizeParams& params, u32 seed,
                   const vector<f32>* deposit) {
    WaterPass w { out, coarse, eroded, macro, hydro, hydroSpec, params,
                  seed, deposit };
    const vector<f32> waterDist = waterDistance(w);
    // Snapshot of the ground before any carve: the river carve references
    // it (never the cells it already dug) and the rock mask reads the
    // carve depth as (preCarve - final).
    const vector<f32> preCarve = out.height;
    carveRiverBeds(w, preCarve);
    carveLakeBasins(w);
    digPlacedPonds(w);
    const vector<Vec2> mouths = collectSeaMouths(w);
    const vector<Vec2> heads = collectSpringHeads(w);
    writeMaskChannels(w, waterDist, preCarve, mouths, heads);
}

FinalizeResult finalizeTerrain(const GridSpec& coarse,
                               const vector<f32>& eroded,
                               const MacroResult& macro,
                               const HydrologyResult& hydro,
                               const GridSpec& hydroSpec,
                               const FinalizeParams& params, u32 seed,
                               const vector<f32>* fineScale,
                               const vector<f32>* deposit,
                               const std::atomic<bool>* cancel) {
    FinalizeResult out =
        finalizeGround(coarse, eroded, macro, &hydro.area, &hydroSpec,
                       params, seed, fineScale, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) {
        return out;
    }
    finalizeWater(out, coarse, eroded, macro, hydro, hydroSpec, params,
                  seed, deposit);
    return out;
}

u64 hashKey(const FinalizeParams& p) {
    core::Fnv64 h;
    reflect::hashReflected(FinalizeParams::staticTypeInfo(), &p, h);
    // Nested params, by its own key — the only member to keep in step
    // with the struct:
    h.mix(hashKey(p.fine));
    return h.value;
}

} // namespace render::terraingen
