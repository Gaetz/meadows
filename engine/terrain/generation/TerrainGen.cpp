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
constexpr u32 kSaltCalm = 0xca1a90c1u;
constexpr u32 kSaltPeakAlpine = 0xa1b13e00u;
constexpr u32 kSaltPeakHill = 0x811c0113u;
constexpr u32 kSaltValleyAxis = 0x7a11e7a5u;
constexpr u32 kSaltTrunk = 0x77201c00u;
constexpr u32 kSaltCol = 0xc0110000u;
constexpr u32 kSaltAxialWarp = 0x51deca5eu;
constexpr u32 kSaltContinentCarrier = 0xc0471e47u;

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

// Valley potential: ONE smooth scalar field drives the whole valley
// system — the trunk valleys are its ISOLINE stripes (continuous
// curves by construction, they wander with the field; the stripe
// index is a global valley identity so one hash rules a valley end to
// end), and the local axis is the isoline direction (perp of the
// gradient). Everything is a pure smooth function of (x, z):
// translation-invariant, no cells, no frames — the earlier
// rotated-frame formulation tore the field apart far from the origin
// (a locally varying angle sweeps r*dTheta of coordinates).
struct ValleyField {
    f32 phi { 0.0f };      // potential [0,1]
    f32 gradLen { 0.0f };  // |grad phi| (per meter)
    f32 axisCos { 1.0f };  // isoline direction (undirected)
    f32 axisSin { 0.0f };
    f32 strength { 0.0f }; // fades at the potential's extrema
};

ValleyField valleyField(u32 seed, f32 x, f32 z, f32 wavelength) {
    const auto phiAt = [&](f32 px, f32 pz) {
        return noise::fbm(seed, px, pz, 1.0f / wavelength, 2, 2.0f,
                          0.5f);
    };
    ValleyField out;
    out.phi = phiAt(x, z);
    const f32 e = 150.0f;
    const f32 gx = (phiAt(x + e, z) - phiAt(x - e, z)) / (2.0f * e);
    const f32 gz = (phiAt(x, z + e) - phiAt(x, z - e)) / (2.0f * e);
    out.gradLen = std::sqrt(gx * gx + gz * gz);
    if (out.gradLen > 1.0e-9f) {
        out.axisCos = -gz / out.gradLen;
        out.axisSin = gx / out.gradLen;
    }
    // Normalized gradient (units of amplitude per wavelength) —
    // band-passed. The masks live in PHASE space (smooth in phi), so
    // the band only bounds the METRIC width of a valley: at the low
    // end the phase stalls and a stripe balloons into a crater-wide
    // patch, at the high end it pinches into a slit — both fade out
    // instead. Kept as open as those bounds allow: the fleuve
    // corridors need CONNECTED valleys to drain along.
    const f32 gn = out.gradLen * wavelength;
    out.strength = noise::smoothstep01(0.08f, 0.2f, gn) *
                   (1.0f - noise::smoothstep01(1.0f, 1.5f, gn));
    return out;
}

// One objective layer: a jittered grid of landmark kernels (alpine
// summits, marked hills, open clearings). fbm cannot promise spacing —
// the grid gives a hard bound on the distance to the nearest landmark,
// stays a pure function of (seed, x, z), and the analytic silhouette
// mirrors it for free (same controls path). The per-cell hash decides
// position, size and SILHOUETTE VARIANT: landmarks must be
// recognizable, not interchangeable.
struct LandmarkSample {
    f32 add { 0.0f };      // meters of base lift (onto plateau)
    f32 clearing { 0.0f }; // [0,1] relief-suppression bowl
};

LandmarkSample landmarkLayer(u32 seed, f32 x, f32 z, f32 cellSize,
                             f32 heightMin, f32 heightMax, f32 radiusMin,
                             f32 radiusMax, bool clearings) {
    LandmarkSample out;
    const i32 cellX = static_cast<i32>(std::floor(x / cellSize));
    const i32 cellZ = static_cast<i32>(std::floor(z / cellSize));
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 gx = cellX + dx;
            const i32 gz = cellZ + dz;
            const auto jitter = [&](u32 k) {
                return noise::lattice(seed + k * 0x9e3779b9u, gx, gz);
            };
            const f32 px =
                (static_cast<f32>(gx) + 0.2f + 0.6f * jitter(0)) *
                cellSize;
            const f32 pz =
                (static_cast<f32>(gz) + 0.2f + 0.6f * jitter(1)) *
                cellSize;
            const f32 height = glm::mix(heightMin, heightMax, jitter(2));
            const f32 radius = glm::mix(radiusMin, radiusMax, jitter(3));
            const f32 variant = jitter(4);
            const bool mesa = !clearings && variant >= 0.7f;
            const bool ridge = !clearings && variant >= 0.4f && !mesa;
            const f32 aspect = ridge
                                   ? glm::mix(2.8f, 4.5f, jitter(5))
                                   : 1.0f + 0.4f * jitter(5);
            const f32 theta = jitter(6) * 3.14159265f;
            const f32 ct = std::cos(theta);
            const f32 st = std::sin(theta);
            const f32 rx = x - px;
            const f32 rz = z - pz;
            const f32 u = (ct * rx + st * rz) / (radius * aspect);
            const f32 v = (-st * rx + ct * rz) / radius;
            const f32 n2 = u * u + v * v;
            if (n2 >= 1.0f) {
                continue;
            }
            if (clearings && variant < 0.3333f) {
                const f32 bowl = (1.0f - n2) * (1.0f - n2);
                out.clearing = glm::max(out.clearing, bowl);
                continue;
            }
            // Dome/ridge: C1 kernel the erosion carves into flanks;
            // mesa: flat top with a short rim (terrace-free — the
            // kernel shape IS the stratum).
            const f32 n = std::sqrt(n2);
            const f32 k =
                mesa ? 1.0f - noise::smoothstep01(0.55f, 0.9f, n)
                     : (1.0f - n2) * (1.0f - n2);
            out.add = glm::max(out.add, height * k);
        }
    }
    return out;
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
    // Anisotropy: smear the oscillating carriers ALONG the local
    // valley axis with an axial domain warp — a LOCAL displacement, so
    // it stays translation-invariant (a rotated/scaled frame with a
    // varying angle tears far from the origin). Strength 0 (tests,
    // painted sources) = legacy.
    f32 awx = wx;
    f32 awz = wz;
    if (s.axisStrength > 0.0f && p.valleyStretch > 1.0f) {
        const f32 amp = t.reliefWavelength * 0.6f *
                        (p.valleyStretch - 1.0f) * s.axisStrength;
        const f32 slide =
            (noise::fbm(seed ^ kSaltAxialWarp, x, z,
                        1.0f / (t.reliefWavelength * 1.7f), 2, 2.0f,
                        0.5f) *
                 2.0f -
             1.0f) *
            amp;
        awx += s.axisCos * slide;
        awz += s.axisSin * slide;
    }
    const f32 relief = (noise::fbm(seed ^ kSaltRelief, awx, awz,
                                   1.0f / t.reliefWavelength, 4, 2.0f,
                                   0.5f) *
                            2.0f -
                        1.0f) *
                       t.reliefAmplitude * s.reliefScale;
    f32 h = t.altitude + relief + s.plateau;
    if (s.hillRelief > 0.0f && hillChainWavelength > 1.0f) {
        // Ridged chains: elongated crests, the erosion pass rounds
        // them into rolling hill country.
        h += noise::ridgedFbm(seed ^ kSaltHillChain, awx, awz,
                              1.0f / hillChainWavelength, 3, 2.0f,
                              0.5f) *
             s.hillRelief * s.reliefScale;
    }
    // Master-valley depression: a wide flat floor dug into whatever
    // stands here (a gorge through a range), fading out near the sea
    // so no inland trough floods below the waterline.
    if (s.trunkDepth > 0.0f) {
        h -= s.trunkDepth *
             noise::smoothstep01(40.0f, 90.0f, h - p.seaLevel);
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
    f32 c = noise::fbm(p.seed ^ kSaltContinent, wx, wz,
                       1.0f / p.continentWavelength, 4, 2.0f, 0.5f);
    // Continental carrier: the very slow field deciding WHERE the
    // land masses and open seas are; the fbm above keeps drawing the
    // local coastline character on top — and only there: where the
    // carrier is decided (well above or below its midline) the local
    // detail is compressed, so continent interiors keep their lakes
    // rare and open oceans their islands rare. The coast detail
    // belongs to the coastal belts. Off (0) = legacy bit-exact.
    if (p.continentCarrierWavelength > 1.0f) {
        // Contrasted: raw fbm hugs its midline, which would leave most
        // of the map in the undecided belt — the remap pushes it to
        // its extremes (solid continent / open ocean) with a narrow
        // coastal transition band where the local detail lives.
        const f32 carrier = noise::smoothstep01(
            0.38f, 0.62f,
            noise::fbm(p.seed ^ kSaltContinentCarrier, x, z,
                       1.0f / p.continentCarrierWavelength, 3, 2.0f,
                       0.5f));
        const f32 lift =
            (carrier - 0.5f) * 2.0f * p.continentCarrierAmp;
        const f32 decided = noise::smoothstep01(
            0.3f * p.continentCarrierAmp, 0.9f * p.continentCarrierAmp,
            std::abs(lift));
        c = 0.5f + (c - 0.5f) * (1.0f - 0.6f * decided) + lift;
    }
    return c;
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
    // Valley potential: the axis relief elongates along and the trunk
    // valleys follow — see valleyField (isoline stripes, translation-
    // invariant).
    const ValleyField valley = valleyField(
        p.seed ^ kSaltValleyAxis, x, z, p.valleyAxisWavelength);
    sample.axisCos = valley.axisCos;
    sample.axisSin = valley.axisSin;
    sample.axisStrength = valley.strength;
    // Master (trunk) valleys: isoline stripes of the potential, one
    // every ~trunkSpacing (in meters, via the local gradient). The
    // stripe index is the valley's identity end to end. Inland-gated;
    // through a range the depression reads as a gorge (uplift keeps
    // the walls, gentle keeps the floor walkable).
    {
        // Stripe step chosen so the typical ISOLINE spacing (dPhi /
        // |grad|) lands near trunkSpacing at the field's typical
        // gradient (~0.35 amplitude per wavelength).
        const f32 stripeStep =
            0.35f * p.trunkSpacing / p.valleyAxisWavelength;
        const f32 phase = valley.phi / stripeStep;
        const i32 stripe =
            static_cast<i32>(std::floor(phase));
        // Distance to the NEAREST neighbouring valley line, measured
        // and masked IN PHASE UNITS — a meters conversion through the
        // local gradient is ill-conditioned where the gradient bends
        // (the 1/|grad| term shears the mask into visible steps). In
        // phase space everything is a smooth function of phi; the
        // metric width of a valley then breathes with the local
        // gradient (wide floors where the field is flat, narrow in
        // steep zones), bounded by the strength band-pass. Checking
        // the three candidate stripes keeps both sides of a stripe
        // boundary in agreement (per-stripe centers differ).
        f32 distPhase = 1.0e9f;
        i32 owner = stripe;
        for (i32 k = stripe - 1; k <= stripe + 1; ++k) {
            const f32 center =
                static_cast<f32>(k) + 0.35f +
                0.3f * noise::lattice(p.seed ^ kSaltTrunk, k, 7);
            const f32 d = std::abs(phase - center);
            if (d < distPhase) {
                distPhase = d;
                owner = k;
            }
        }
        const f32 floorPhase =
            p.trunkFloorHalfWidth / p.trunkSpacing;
        const f32 shoulderPhase = p.trunkShoulder / p.trunkSpacing;
        const f32 profile = 1.0f - noise::smoothstep01(
                                       floorPhase, shoulderPhase,
                                       distPhase);
        const f32 depth = glm::mix(
            p.trunkDepthMin, p.trunkDepthMax,
            noise::lattice(p.seed ^ kSaltTrunk ^ 0x5bd1e995u, owner,
                           113));
        // Own WIDE inland ramp: the shared `inland` gate rides the
        // warped continent field and snaps 0->1 within tens of meters
        // at the coast — fine for additive lifts, a visible tear for
        // a mask. A full tier of ramp spreads it over ~a kilometer.
        const f32 trunkInland =
            noise::smoothstep01(0.3f, 1.3f, sample.tier);
        const f32 gate = trunkInland * valley.strength;
        sample.trunk = (1.0f - noise::smoothstep01(
                                   floorPhase * 0.8f,
                                   floorPhase * 1.3f, distPhase)) *
                       gate;
        sample.trunkDepth = depth * profile * gate;
    }
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
    // Guaranteed cols: thin stripes of a SECOND potential cut across
    // the country every ~colSpacing wherever ranges rise — no range is
    // ever a regional wall. And the trunk floor is itself a corridor:
    // the walkway of the future fleuve.
    {
        const f32 psi = noise::fbm(p.seed ^ kSaltCol, x, z,
                                   1.0f / (p.colSpacing * 3.2f), 2,
                                   2.0f, 0.5f);
        const f32 colPhase = psi * 3.2f;
        const f32 frac = colPhase - std::floor(colPhase);
        const f32 colK =
            1.0f -
            noise::smoothstep01(0.045f, 0.13f, std::abs(frac - 0.5f));
        sample.gentle =
            glm::max(sample.gentle, colK * rangeNeed);
    }
    sample.gentle = glm::max(sample.gentle, 0.9f * sample.trunk);
    // Lithology: a slow hardness field — hard pockets keep sharp
    // relief and cliff coasts, soft pockets roll. Plain fbm: the mean
    // sits at neutral, the tails are the drama.
    sample.hardness =
        noise::fbm(p.seed ^ kSaltHardness, x, z,
                   1.0f / p.hardnessWavelength, 3, 2.0f, 0.5f);
    // Calm socles: the habitable family — true plains (no orogeny, no
    // hill chains, softish rock) thinned by a slow band so some plains
    // stay rugged, plus the flat tops of swelled highland plateaus.
    // Corridors are members by definition. Valley floors join after
    // erosion (tile bake) — they are unknowable pointwise.
    const f32 plainW =
        (1.0f - noise::smoothstep01(0.03f, 0.12f, sample.uplift)) *
        (1.0f - noise::smoothstep01(25.0f, 60.0f, sample.hillRelief)) *
        (1.0f - noise::smoothstep01(0.62f, 0.8f, sample.hardness));
    const f32 calmBand = noise::smoothstep01(
        0.36f, 0.52f,
        noise::fbm(p.seed ^ kSaltCalm, x, z, 1.0f / p.calmWavelength, 3,
                   2.0f, 0.5f));
    const f32 plateauTopW =
        noise::smoothstep01(90.0f, 200.0f, sample.plateau) *
        (1.0f - noise::smoothstep01(35.0f, 80.0f, sample.hillRelief)) *
        (1.0f - noise::smoothstep01(0.15f, 0.4f, sample.uplift));
    sample.calm =
        glm::max(sample.gentle,
                 glm::max(plainW, plateauTopW) * calmBand);
    // Objective layers (jittered landmark grids): added to the base
    // lift so the erosion keep protects the summit while the flanks
    // stay carved (the alpine character). Faded where the swell/massif
    // anchors already carry high ground, inland-gated like the swell.
    // NOTE: calm above reads the PRE-landmark plateau on purpose — a
    // cone summit is not a calm plateau top.
    const LandmarkSample alpinePeaks = landmarkLayer(
        p.seed ^ kSaltPeakAlpine, x, z, p.peakCellSize, p.peakHeightMin,
        p.peakHeightMax, p.peakRadiusMin, p.peakRadiusMax, false);
    const LandmarkSample hillMarks = landmarkLayer(
        p.seed ^ kSaltPeakHill, x, z, p.hillCellSize, p.hillHeightMin,
        p.hillHeightMax, p.hillRadiusMin, p.hillRadiusMax, true);
    sample.plateau +=
        alpinePeaks.add * inland *
            (1.0f - noise::smoothstep01(450.0f, 750.0f, sample.plateau)) +
        hillMarks.add * inland *
            (1.0f - noise::smoothstep01(60.0f, 120.0f, sample.hillRelief));
    // A clearing is a DESIGNED socle: flatten the relief carriers in
    // the bowl and hand the ground to the calm family. Walk-scale
    // country only — on active orogeny the flat shelves are the
    // `gentle` corridors' job, never a decree against the fastscape.
    const f32 clearing =
        hillMarks.clearing * inland *
        (1.0f - noise::smoothstep01(0.25f, 0.5f, sample.uplift));
    // Trunk floors flatten their carriers too — a master valley floor
    // is open ground, not a corrugated trench.
    sample.reliefScale =
        (1.0f - 0.75f * clearing) * (1.0f - 0.6f * sample.trunk);
    sample.calm = glm::max(sample.calm, clearing);
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
                            u32 seed) {
    MacroResult out;
    out.spec = spec;
    out.height.resize(spec.cells());
    out.uplift.resize(spec.cells());
    out.biome.resize(spec.cells());
    out.gentle.resize(spec.cells());
    out.calm.resize(spec.cells());
    out.trunk.resize(spec.cells());
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
            out.calm[i] = s.sea ? 0.0f : s.calm;
            out.trunk[i] = s.sea ? 0.0f : s.trunk;
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
                           params.hillChainWavelength, spec.x(col),
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
    const f32 calmHigh =
        s.calm * glm::smoothstep(150.0f, 400.0f, h - params.seaLevel);
    const f32 keep = glm::min(kPlateauKeepMax,
                              s.plateau * kPlateauKeepCoef +
                                  calmHigh * kCalmKeep);
    const f32 threshold = 80.0f + 1100.0f * keep;
    const f32 rel = h - params.seaLevel;
    if (rel <= threshold) {
        return h;
    }
    return params.seaLevel + threshold + 0.25f * (rel - threshold);
}

} // namespace render::terraingen
