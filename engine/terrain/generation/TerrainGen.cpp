#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/GridOps.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/Noise.hpp"

namespace render::terraingen {

namespace {

// Seed salts: one per independent noise field, so a control tweak never
// re-rolls an unrelated field.
constexpr u32 kSaltContinent = 0x5ea5c0a5u;
constexpr u32 kSaltContinentWarpX = 0xa1b2c3d4u;
constexpr u32 kSaltContinentWarpZ = 0xb7c8d9eau;
constexpr u32 kSaltUplift = 0x0f1e2d3cu;
constexpr u32 kSaltTemperature = 0x7ea7be57u;
constexpr u32 kSaltMoisture = 0x6d015745u;
constexpr u32 kSaltRelief = 0xe5f6a7b8u;
constexpr u32 kSaltReliefWarpX = 0xc3d4e5f6u;
constexpr u32 kSaltReliefWarpZ = 0xd9eafb0cu;
constexpr u32 kSaltRegime = 0x4b1d5eedu;
constexpr u32 kSaltHillChain = 0x91c0ffeeu;
constexpr u32 kSaltSwell = 0x5e110000u;
constexpr u32 kSaltGentle = 0x6e97e155u;
constexpr u32 kSaltHardness = 0x11780c1cu;

struct TierBlend {
    f32 altitude;
    f32 reliefAmplitude;
    f32 reliefWavelength;
    f32 terrace;
};

TierBlend blendTiers(const MacroParams& p, f32 tier) {
    const f32 last = static_cast<f32>(p.tiers.size() - 1);
    const f32 ti = glm::clamp(tier, 0.0f, last);
    const size_t i0 = static_cast<size_t>(ti);
    const size_t i1 = glm::min(i0 + 1, p.tiers.size() - 1);
    const f32 t = ti - static_cast<f32>(i0);
    // Smoothstep the blend so tier floors read as floors with a shoulder
    // between them, not one long ramp.
    const f32 tt = t * t * (3.0f - 2.0f * t);
    const TierLevel& a = p.tiers[i0];
    const TierLevel& b = p.tiers[i1];
    return { glm::mix(a.altitude, b.altitude, tt),
             glm::mix(a.reliefAmplitude, b.reliefAmplitude, tt),
             glm::mix(a.reliefWavelength, b.reliefWavelength, tt),
             glm::mix(a.terrace, b.terrace, tt) };
}

// Land surface before the coast profile: tier floor + warped relief +
// soft strata quantization, plus the regime extras — the old-massif
// plateau and the ridged hill-chain relief (0/0 = legacy).
f32 landHeight(const MacroParams& p, u32 seed, const ControlSample& s,
               f32 hillChainWavelength, f32 x, f32 z) {
    const f32 tier = s.tier;
    const TierBlend t = blendTiers(p, tier);
    const f32 wx =
        x + (noise::fbm(seed ^ kSaltReliefWarpX, x, z,
                        1.0f / p.warpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.warpStrength;
    const f32 wz =
        z + (noise::fbm(seed ^ kSaltReliefWarpZ, x, z,
                        1.0f / p.warpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.warpStrength;
    const f32 relief = (noise::fbm(seed ^ kSaltRelief, wx, wz,
                                   1.0f / t.reliefWavelength, 4, 2.0f,
                                   0.5f) *
                            2.0f -
                        1.0f) *
                       t.reliefAmplitude;
    f32 h = t.altitude + relief + s.plateau;
    if (s.hillRelief > 0.0f && hillChainWavelength > 1.0f) {
        // Ridged chains: elongated crests, the erosion pass rounds
        // them into rolling hill country.
        h += noise::ridgedFbm(seed ^ kSaltHillChain, wx, wz,
                              1.0f / hillChainWavelength, 3, 2.0f,
                              0.5f) *
             s.hillRelief;
    }
    if (t.terrace > 0.0f && p.terraceStep > 0.0f) {
        // Soft quantization: flat strata with a short warped slope at
        // each step edge — mesas, not ziggurats (the relief warp above
        // already bends the contour lines).
        const f32 cell = std::floor(h / p.terraceStep);
        const f32 frac = h / p.terraceStep - cell;
        const f32 edge = glm::clamp(p.terraceEdge, 0.01f, 0.49f);
        const f32 soft =
            noise::smoothstep01(0.5f - edge, 0.5f + edge, frac);
        const f32 q = (cell + soft) * p.terraceStep;
        h = glm::mix(h, q, t.terrace);
    }
    return h;
}

// Coast profile from the signed shore distance (+ on land, meters).
// Continuous at d == 0 (both sides meet at shoreHeight above sea level);
// high tiers AND hard-rock coasts skip the beach ramp and keep their
// altitude to the rim (calanques / chalk cliffs), with a narrower shelf
// and a steeper underwater plunge.
f32 coastProfile(const MacroParams& p, f32 land, f32 tier, f32 d,
                 f32 hardness) {
    const f32 waterline = p.seaLevel + p.shoreHeight;
    const f32 cliff =
        glm::max(noise::smoothstep01(p.cliffTierStart, p.cliffTierEnd,
                                     tier),
                 noise::smoothstep01(0.62f, 0.8f, hardness));
    if (d <= 0.0f) {
        const f32 shelf = glm::mix(p.shelfWidth, p.shelfWidth * 0.35f,
                                   cliff);
        const f32 falloff =
            glm::mix(p.seaFalloff, p.seaFalloff * 0.4f, cliff);
        const f32 shallow =
            glm::mix(waterline, p.seaLevel - p.shallowDepth,
                     noise::smoothstep01(0.0f, shelf, -d));
        return glm::mix(shallow, p.seaFloor,
                        noise::smoothstep01(shelf, falloff, -d));
    }
    const f32 ramp =
        glm::mix(waterline, land,
                 noise::smoothstep01(0.0f, p.shoreWidth, d));
    return glm::mix(ramp, land, cliff);
}

// Two-pass 3x3 chamfer distance transform of the sea mask, signed in
// meters: + on land (distance to sea), - at sea (distance to land).
vector<f32> signedSeaDistance(const GridSpec& spec,
                              const vector<u8>& seaMask) {
    const i32 n = static_cast<i32>(spec.n);
    constexpr f32 kFar = 1.0e30f;
    vector<f32> toSea(spec.cells(), kFar);
    vector<f32> toLand(spec.cells(), kFar);
    const auto idx = [n](i32 cx, i32 cz) {
        return static_cast<size_t>(cz) * static_cast<size_t>(n) + cx;
    };
    for (i32 cz = 0; cz < n; ++cz) {
        for (i32 cx = 0; cx < n; ++cx) {
            (seaMask[idx(cx, cz)] ? toSea : toLand)[idx(cx, cz)] = 0.0f;
        }
    }
    chamferSweep(toSea, n, n);
    chamferSweep(toLand, n, n);
    vector<f32> out(spec.cells());
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = (seaMask[i] ? -toLand[i] : toSea[i]) * spec.texelSize;
    }
    return out;
}

} // namespace

f32 recurveLand(const MacroParams& p, f32 h) {
    if (p.recurveLow == 0.25f && p.recurveMid == 0.5f &&
        p.recurveHigh == 0.75f) {
        return h; // identity: bit-exact
    }
    const f32 span = glm::max(p.recurveSpan, 1.0f);
    const f32 t = (h - p.seaLevel) / span;
    if (t <= 0.0f || t >= 1.0f) {
        return h; // sea/shoreline and above-span land untouched
    }
    // Clamp the control points into a strictly increasing sequence so
    // the curve stays monotone whatever the data says.
    const f32 lo = glm::clamp(p.recurveLow, 0.02f, 0.96f);
    const f32 mid = glm::clamp(p.recurveMid, lo + 0.01f, 0.97f);
    const f32 hi = glm::clamp(p.recurveHigh, mid + 0.01f, 0.98f);
    const f32 y[5] = { 0.0f, lo, mid, hi, 1.0f };
    // Monotone PCHIP (Fritsch-Carlson): harmonic-mean interior slopes on
    // uniform knots at 0, 1/4, 1/2, 3/4, 1.
    f32 d[4];
    for (i32 k = 0; k < 4; ++k) {
        d[k] = (y[k + 1] - y[k]) * 4.0f;
    }
    f32 m[5];
    m[0] = d[0];
    m[4] = d[3];
    for (i32 k = 1; k < 4; ++k) {
        m[k] = 2.0f / (1.0f / d[k - 1] + 1.0f / d[k]);
    }
    const i32 seg = glm::min(static_cast<i32>(t * 4.0f), 3);
    const f32 s = t * 4.0f - static_cast<f32>(seg);
    const f32 s2 = s * s;
    const f32 s3 = s2 * s;
    const f32 out = (2.0f * s3 - 3.0f * s2 + 1.0f) * y[seg] +
                    (s3 - 2.0f * s2 + s) * 0.25f * m[seg] +
                    (-2.0f * s3 + 3.0f * s2) * y[seg + 1] +
                    (s3 - s2) * 0.25f * m[seg + 1];
    return p.seaLevel + out * span;
}

f32 ProceduralControls::continentalness(f32 x, f32 z) const {
    const f32 wx =
        x + (noise::fbm(p.seed ^ kSaltContinentWarpX, x, z,
                        1.0f / p.warpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.warpStrength;
    const f32 wz =
        z + (noise::fbm(p.seed ^ kSaltContinentWarpZ, x, z,
                        1.0f / p.warpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.warpStrength;
    return noise::fbm(p.seed ^ kSaltContinent, wx, wz,
                      1.0f / p.continentWavelength, 4, 2.0f, 0.5f);
}

ControlSample ProceduralControls::at(f32 x, f32 z) const {
    const f32 c = continentalness(x, z);
    ControlSample sample;
    sample.sea = c < p.seaThreshold;
    sample.tier = glm::clamp((c - p.seaThreshold) / p.tierSpread, 0.0f,
                             1.0f) *
                  p.maxTier;
    // Ranges rise where the ridged mask fires, and prefer high ground —
    // uplift feeds stage S2 (stream-power erosion), it is not height.
    const f32 mask = noise::smoothstep01(
        p.upliftMaskLow, p.upliftMaskHigh,
        noise::ridgedFbm(p.seed ^ kSaltUplift, x, z,
                         1.0f / p.upliftWavelength, 3, 2.0f, 0.5f));
    sample.uplift = mask * noise::smoothstep01(0.0f, 1.2f, sample.tier);
    // Relief regime: a slow field sorts the land into hill-chain
    // country / old massifs / young ranges. Inland-gated so coasts keep
    // their shore profile whatever the regime says.
    const f32 regime = noise::fbm(p.seed ^ kSaltRegime, x, z,
                                  1.0f / p.regimeWavelength, 3, 2.0f,
                                  0.5f);
    const f32 hills = 1.0f - noise::smoothstep01(0.22f, 0.33f, regime);
    const f32 old = noise::smoothstep01(0.55f, 0.68f, regime);
    const f32 inland = noise::smoothstep01(0.15f, 0.6f, sample.tier);
    // Hill country stays LOW (no ranges) and rolls; old massifs stand
    // on a plateau wearing hills, their uplift nearly off — erosion
    // rounds what little rises. Young ranges keep the plain path.
    sample.uplift *= (1.0f - hills) * (1.0f - 0.85f * old);
    sample.tier = glm::mix(sample.tier,
                           glm::min(sample.tier, 1.1f), hills);
    sample.tier =
        glm::mix(sample.tier, glm::min(sample.tier, 1.4f), old);
    sample.plateau = old * inland * p.oldMassifHeight;
    sample.hillRelief =
        inland * (hills * p.hillChainAmplitude +
                  old * p.oldMassifHillAmplitude);
    // A gentle uplift FLOOR keeps regime hills alive through S2: the
    // range mask fires nowhere near them, so without this the ridged
    // S1 hills erode to a featureless plain in a hundred iterations —
    // an old massif must stay hilly, only rounded.
    sample.uplift = glm::max(
        sample.uplift,
        inland * glm::max(old * 0.04f, hills * 0.03f));
    // The LONG swell: whole landscapes ride a very slow positive lift —
    // ranges on it reach true high-mountain altitudes, hill country on
    // it reads as highland plateau. Positive-only (it raises, never
    // digs) and inland-gated so coasts keep their profile.
    const f32 swell = noise::smoothstep01(
        0.45f, 0.85f,
        noise::fbm(p.seed ^ kSaltSwell, x, z, 1.0f / p.swellWavelength,
                   3, 2.0f, 0.5f));
    sample.plateau += swell * inland * p.swellHeight;
    // Passability corridors: erosion softeners, never height. Banded so
    // roughly a quarter of the land is a gentle passage — except in the
    // ranges, where the band widens with the uplift the walker must
    // cross: drama stays, but always with a way through.
    const f32 rangeNeed =
        noise::smoothstep01(0.2f, 0.8f, sample.uplift);
    sample.gentle = noise::smoothstep01(
        0.55f - 0.18f * rangeNeed, 0.7f - 0.18f * rangeNeed,
        noise::fbm(p.seed ^ kSaltGentle, x, z,
                   1.0f / p.gentleWavelength, 3, 2.0f, 0.5f));
    // Lithology: a slow hardness field — hard pockets keep sharp
    // relief and cliff coasts, soft pockets roll. Plain fbm: the mean
    // sits at neutral, the tails are the drama.
    sample.hardness =
        noise::fbm(p.seed ^ kSaltHardness, x, z,
                   1.0f / p.hardnessWavelength, 3, 2.0f, 0.5f);
    // Climate -> biome id (palette contract in ProceduralControlParams):
    // cold beats arid beats alpine; temperate is the default.
    const f32 temperature =
        noise::fbm(p.seed ^ kSaltTemperature, x, z,
                   1.0f / p.climateWavelength, 3, 2.0f, 0.5f);
    const f32 moisture =
        noise::fbm(p.seed ^ kSaltMoisture, x, z,
                   1.0f / p.climateWavelength, 3, 2.0f, 0.5f);
    if (temperature < 0.34f) {
        sample.biome = 3; // tundra
    } else if (moisture < 0.38f && temperature > 0.58f) {
        sample.biome = 1; // arid
    } else if (sample.tier > 2.1f) {
        sample.biome = 2; // alpine
    }
    return sample;
}

MacroResult synthesizeMacro(const ControlSource& controls,
                            const GridSpec& spec, const MacroParams& params,
                            u32 seed, f32 hillChainWavelength) {
    MacroResult out;
    out.spec = spec;
    out.height.resize(spec.cells());
    out.uplift.resize(spec.cells());
    out.biome.resize(spec.cells());
    out.gentle.resize(spec.cells());
    out.plateau.resize(spec.cells());
    out.hillRelief.resize(spec.cells());
    out.hardness.resize(spec.cells());
    vector<ControlSample> samples(spec.cells());
    vector<u8> seaMask(spec.cells());
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            const ControlSample s = controls.at(spec.x(col), spec.z(row));
            samples[i] = s;
            seaMask[i] = s.sea ? 1 : 0;
            out.uplift[i] = s.sea ? 0.0f : s.uplift;
            out.biome[i] = s.biome;
            out.gentle[i] = s.gentle;
            out.plateau[i] = s.sea ? 0.0f : s.plateau;
            out.hillRelief[i] = s.sea ? 0.0f : s.hillRelief;
            out.hardness[i] = s.hardness;
        }
    }
    out.seaDist = signedSeaDistance(spec, seaMask);
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            const f32 land = recurveLand(
                params,
                landHeight(params, seed, samples[i],
                           hillChainWavelength, spec.x(col),
                           spec.z(row)));
            out.height[i] =
                coastProfile(params, land, samples[i].tier,
                             out.seaDist[i], samples[i].hardness);
        }
    }
    return out;
}

f32 macroHeightAnalytic(const ProceduralControls& controls,
                        const MacroParams& params, f32 x, f32 z) {
    const ControlSample s = controls.at(x, z);
    const f32 land = recurveLand(
        params, landHeight(params, controls.params().seed, s,
                           controls.params().hillChainWavelength, x, z));
    // Shore distance approximated from continentalness: the ramp of the
    // tier mapping doubles as a distance proxy (good enough for
    // silhouettes and boundary conditions).
    const f32 c = controls.continentalness(x, z);
    const f32 d = (c - controls.params().seaThreshold) *
                  controls.params().continentWavelength * 0.35f;
    const f32 h = coastProfile(params, land, s.tier, d, s.hardness);
    // Erosion-aware silhouette: the fastscape carves the macro back
    // toward base level, sparing what the plateau keep protects. Far
    // meshes and the out-of-region fallback sample THIS surface, so a
    // distant summit must sit near its future baked height, not at the
    // pre-erosion macro's promise (which runs up to ~350 m proud).
    // Piecewise-linear fit of the baked-vs-analytic calibration
    // (tests: 'erosion calibration'): heights are kept up to a keep-
    // dependent threshold, then compressed to the measured tail slope.
    const f32 keep =
        glm::min(kPlateauKeepMax, s.plateau * kPlateauKeepCoef);
    const f32 threshold = 80.0f + 1100.0f * keep;
    const f32 rel = h - params.seaLevel;
    if (rel <= threshold) {
        return h;
    }
    return params.seaLevel + threshold + 0.25f * (rel - threshold);
}

} // namespace render::terraingen
