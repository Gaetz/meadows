#include "engine/terrain/generation/WorldLayer.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/terrain/Noise.hpp"

namespace render::terraingen {

namespace {

// One salt per field: a tuning of one never re-rolls another.
constexpr u32 kSaltContinent = 0x5ea5c0a5u;
constexpr u32 kSaltWarpX = 0xa1b2c3d4u;
constexpr u32 kSaltWarpZ = 0xb7c8d9eau;
constexpr u32 kSaltCoastDetail = 0xc0a57de7u;
constexpr u32 kSaltEtage = 0xe7a9e000u;
constexpr u32 kSaltMassif = 0x3a551f00u;
constexpr u32 kSaltBench = 0xbe9c4000u;
constexpr u32 kSaltPlateau = 0x91a7ea00u;
constexpr u32 kSaltTemperature = 0x7ea7be57u;
constexpr u32 kSaltMoisture = 0x6d015745u;
constexpr u32 kSaltTemperatureSlow = 0x7ea7be58u;
constexpr u32 kSaltMoistureSlow = 0x6d015746u;
constexpr u32 kSaltCoverT = 0xc0ec0e01u;
constexpr u32 kSaltCoverM = 0xc0ec0e02u;

// Province altitude from the raw étage field: a monotone table, the
// low end wide (most land is low country), the high end rare.
f32 provinceAltitude(f32 e) {
    constexpr f32 kE[7] = { 0.0f, 0.40f, 0.52f, 0.60f, 0.72f, 0.85f, 1.0f };
    constexpr f32 kA[7] = { 10.0f,  50.0f,  150.0f, 300.0f,
                            520.0f, 800.0f, 1100.0f };
    if (e <= kE[0]) {
        return kA[0];
    }
    for (u32 i = 1; i < 7; ++i) {
        if (e <= kE[i]) {
            // Linear: a smoothstep per segment steepens its middle
            // by half again, and the floor's slope is the walker's.
            const f32 t = (e - kE[i - 1]) / (kE[i] - kE[i - 1]);
            return glm::mix(kA[i - 1], kA[i], t);
        }
    }
    return kA[6];
}

// The raw (contrasted) étage field at a point.
f32 etageRaw(const WorldLayerParams& p, f32 x, f32 z) {
    const f32 wx =
        x + (noise::fbm(p.seed ^ kSaltWarpX, x, z,
                        1.0f / p.continentWarpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.continentWarpStrength;
    const f32 wz =
        z + (noise::fbm(p.seed ^ kSaltWarpZ, x, z,
                        1.0f / p.continentWarpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.continentWarpStrength;
    const f32 e = noise::fbm(p.seed ^ kSaltEtage, wx, wz,
                             1.0f / p.etageWavelength, 3, 2.0f, 0.45f);
    return 0.5f + (e - 0.5f) * 1.3f;
}

// The étage shift that anchors the start: memoized on the whole
// params hash — pure, thread-local, a few floats.
f32 etageAnchorShift(const WorldLayerParams& p) {
    thread_local u64 memoKey = 0;
    thread_local f32 memoShift = 0.0f;
    thread_local bool valid = false;
    const u64 key = hashParams(p);
    if (!valid || memoKey != key) {
        memoKey = key;
        memoShift = p.startEtage - etageRaw(p, p.startX, p.startZ);
        valid = true;
    }
    return memoShift;
}

} // namespace

WorldSample worldSampleAt(const WorldLayerParams& p, f32 x, f32 z) {
    WorldSample w;
    // Warped carrier: coasts and provinces wander, no fbm grid shows.
    const f32 wx =
        x + (noise::fbm(p.seed ^ kSaltWarpX, x, z,
                        1.0f / p.continentWarpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.continentWarpStrength;
    const f32 wz =
        z + (noise::fbm(p.seed ^ kSaltWarpZ, x, z,
                        1.0f / p.continentWarpWavelength, 2, 2.0f, 0.5f) *
                 2.0f -
             1.0f) *
                p.continentWarpStrength;
    f32 c = noise::fbm(p.seed ^ kSaltContinent, wx, wz,
                       1.0f / p.continentWavelength, 3, 2.0f, 0.5f);
    // Contrast: raw fbm hugs its midline; stretched so land and sea
    // are decided and the coast belt stays a belt.
    c = 0.5f + (c - 0.5f) * 1.6f;
    // Start decree: land, low, temperate around the start. `pull` is
    // the meadow, `pullLow` the low country around it.
    const f32 dStart = std::hypot(x - p.startX, z - p.startZ);
    const f32 pull =
        1.0f - noise::smoothstep01(p.startRadius,
                                   p.startRadius + p.startFade, dStart);
    const f32 pullLow =
        1.0f - noise::smoothstep01(p.startLowRadius,
                                   p.startLowRadius + p.startLowFade,
                                   dStart);
    c = glm::mix(c, glm::max(c, p.seaThreshold + 0.14f), pullLow);
    // The carrier alone paces the inland ramp below: the coast detail
    // is a 5 km field, a floor ramped on it would cliff.
    const f32 carrier = c;
    // Coast detail on the belt only: bays, headlands, islets.
    const f32 belt =
        1.0f - noise::smoothstep01(0.0f, 0.16f, std::abs(c - p.seaThreshold));
    c += (noise::fbm(p.seed ^ kSaltCoastDetail, wx, wz,
                     1.0f / p.coastDetailWavelength, 3, 2.0f, 0.5f) -
          0.5f) *
         p.coastDetailAmp * belt;
    w.continent = c;
    w.sea = c < p.seaThreshold;
    w.coast = 1.0f - noise::smoothstep01(0.0f, p.coastBand,
                                         std::abs(c - p.seaThreshold));
    // The étage at the warped point (provinces wander with the coasts),
    // re-based on the start (the anchor fades out far away).
    f32 e = noise::fbm(p.seed ^ kSaltEtage, wx, wz, 1.0f / p.etageWavelength,
                       3, 2.0f, 0.45f);
    e = 0.5f + (e - 0.5f) * 1.3f;
    const f32 anchor =
        1.0f - noise::smoothstep01(p.anchorRadius,
                                   p.anchorRadius + p.anchorFade, dStart);
    e += etageAnchorShift(p) * anchor;
    w.etage = glm::clamp(e, 0.0f, 1.0f);
    // Massif belts: ridges of a slow field, only on the higher
    // provinces, never at the start.
    const f32 massifRaw = noise::fbm(p.seed ^ kSaltMassif, wx, wz,
                                     1.0f / p.massifWavelength, 3, 2.0f,
                                     0.5f);
    w.massif = noise::smoothstep01(0.50f, 0.74f, massifRaw) *
               noise::smoothstep01(0.45f, 0.62f, w.etage) *
               (1.0f - pullLow);
    // The floor: province altitude plus the massif lift, ramped up
    // from the carrier's shoreline (a wider ramp under high provinces
    // keeps the coastal escarpment walkable; land the coast detail
    // adds past the carrier's shore stays low); sea floors sink with
    // the carrier.
    if (w.sea) {
        w.base = -70.0f * noise::smoothstep01(0.0f, 0.25f, p.seaThreshold - c);
    } else {
        f32 alt = provinceAltitude(w.etage);
        // Benches: the floor breathes inside its province.
        alt *= 1.0f + p.benchAmp *
                          (noise::fbm(p.seed ^ kSaltBench, wx, wz,
                                      1.0f / p.benchWavelength, 2, 2.0f,
                                      0.5f) *
                               2.0f -
                           1.0f);
        alt += p.massifLift * w.massif;
        // The meadow: the first steps are flat and low (the tier
        // relief and the beds give its rivers their texture).
        alt = glm::mix(alt, glm::min(80.0f, 40.0f + (alt - 40.0f) * 0.06f),
                       pull);
        // Plateaus: the short étage, stepped (the floor) and ramped
        // (baseSmooth, for the corridors), past the meadow.
        f32 stepped = 0.0f;
        f32 ramped = 0.0f;
        f32 scarp = 0.0f;
        if (p.plateauLevels > 0 && p.plateauStep > 0.0f) {
            const f32 n = static_cast<f32>(p.plateauLevels);
            f32 v = noise::fbm(p.seed ^ kSaltPlateau, wx, wz,
                               1.0f / p.plateauWavelength, 3, 2.0f, 0.5f);
            v = glm::clamp(0.5f + (v - 0.5f) * 1.8f, 0.0f, 1.0f);
            const f32 t = v * n;
            const f32 cell = std::floor(t);
            const f32 frac = t - cell;
            const f32 edge = glm::clamp(p.plateauEdge, 0.01f, 0.49f);
            const f32 soft =
                noise::smoothstep01(0.5f - edge, 0.5f + edge, frac);
            const f32 gate = noise::smoothstep01(
                p.startRadius + p.plateauStartGap * 0.25f,
                p.startRadius + p.plateauStartGap, dStart);
            stepped = glm::min(cell + soft, n) * p.plateauStep * gate;
            ramped = t * p.plateauStep * gate;
            scarp = (cell < n ? 1.0f - glm::clamp(std::abs(frac - 0.5f) / edge,
                                                  0.0f, 1.0f)
                              : 0.0f) *
                    gate;
        }
        const f32 inland = noise::smoothstep01(
            0.0f, p.coastBand * (1.5f + 4.0f * w.etage),
            carrier - p.seaThreshold);
        w.base = (alt + stepped) * inland;
        w.baseSmooth = (alt + ramped) * inland;
        w.scarp = scarp * noise::smoothstep01(0.3f, 0.8f, inland);
    }
    // Climate: regional + continental drift, altitude lapse, wetter
    // coasts; the start is pulled to temperate means.
    const f32 tRegional = noise::fbm(p.seed ^ kSaltTemperature, x, z,
                                     1.0f / p.climateWavelength, 4, 2.0f,
                                     0.5f);
    const f32 tSlow = noise::fbm(p.seed ^ kSaltTemperatureSlow, x, z,
                                 1.0f / p.climateSlowWavelength, 2, 2.0f,
                                 0.5f);
    const f32 mRegional = noise::fbm(p.seed ^ kSaltMoisture, x, z,
                                     1.0f / p.climateWavelength, 4, 2.0f,
                                     0.5f);
    const f32 mSlow = noise::fbm(p.seed ^ kSaltMoistureSlow, x, z,
                                 1.0f / p.climateSlowWavelength, 2, 2.0f,
                                 0.5f);
    f32 temperature = 0.5f + 0.44f * (tRegional - 0.5f) +
                      0.28f * (tSlow - 0.5f) -
                      p.lapsePerKm * glm::max(w.base, 0.0f) / 1000.0f;
    f32 moisture = 0.5f + 0.44f * (mRegional - 0.5f) +
                   0.2f * (mSlow - 0.5f) + 0.15f * w.coast;
    temperature = glm::mix(temperature, 0.50f, pullLow);
    moisture = glm::mix(moisture, 0.55f, pullLow);
    // The cover selector: nudges the climate and, inside the temperate
    // default, picks the variant (paletteIdFor) — start included (a
    // meadow with its heath and its copses is still a meadow).
    const f32 coverT = noise::fbm(p.seed ^ kSaltCoverT, x, z,
                                  1.0f / p.coverWavelength, 2, 2.0f, 0.5f);
    const f32 coverM = noise::fbm(p.seed ^ kSaltCoverM, x, z,
                                  1.0f / p.coverWavelength, 2, 2.0f, 0.5f);
    temperature += p.coverAmp * (coverT * 2.0f - 1.0f);
    moisture += p.coverAmp * (coverM * 2.0f - 1.0f);
    w.cover = coverT;
    w.temperature = temperature;
    w.moisture = moisture;
    return w;
}

u64 hashParams(const WorldLayerParams& p) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    const auto f = [&](f32 v) { mix(&v, sizeof(v)); };
    const auto u = [&](u32 v) { mix(&v, sizeof(v)); };
    u(p.seed);
    f(p.continentWavelength);
    f(p.coastDetailWavelength);
    f(p.coastDetailAmp);
    f(p.continentWarpWavelength);
    f(p.continentWarpStrength);
    f(p.seaThreshold);
    f(p.coastBand);
    f(p.etageWavelength);
    f(p.massifWavelength);
    for (const f32 a : p.etageAltitude) {
        f(a);
    }
    f(p.massifLift);
    f(p.benchWavelength);
    f(p.benchAmp);
    f(p.plateauWavelength);
    f(p.plateauStep);
    u(p.plateauLevels);
    f(p.plateauEdge);
    f(p.plateauStartGap);
    f(p.startX);
    f(p.startZ);
    f(p.startEtage);
    f(p.anchorRadius);
    f(p.anchorFade);
    f(p.startLowRadius);
    f(p.startLowFade);
    f(p.startRadius);
    f(p.startFade);
    f(p.climateWavelength);
    f(p.climateSlowWavelength);
    f(p.lapsePerKm);
    f(p.coverWavelength);
    f(p.coverAmp);
    return h;
}

f32 etageIndexFor(const WorldLayerParams& p, f32 base) {
    if (base <= p.etageAltitude[0]) {
        return 0.0f;
    }
    for (u32 i = 1; i < 4; ++i) {
        if (base <= p.etageAltitude[i]) {
            return static_cast<f32>(i - 1) +
                   (base - p.etageAltitude[i - 1]) /
                       (p.etageAltitude[i] - p.etageAltitude[i - 1]);
        }
    }
    return 3.0f;
}

f32 etageAltitudeFor(const WorldLayerParams& p, f32 tier) {
    const f32 t = glm::clamp(tier, 0.0f, 3.0f);
    const u32 i0 = glm::min(static_cast<u32>(t), 2u);
    return glm::mix(p.etageAltitude[i0], p.etageAltitude[i0 + 1],
                    t - static_cast<f32>(i0));
}

u8 paletteIdFor(f32 temperature, f32 moisture, f32 base, f32 cover,
                u8 characterPalette) {
    if (temperature < 0.34f) {
        return 3; // tundra
    }
    if (moisture < 0.38f && temperature > 0.58f) {
        return 1; // arid
    }
    if (base >= 950.0f) {
        return 2; // alpine
    }
    if (base >= 650.0f) {
        return 4; // subalpine
    }
    if (moisture < 0.46f && temperature > 0.54f) {
        return 5; // steppe
    }
    // Temperate: the character region names its palette; else the
    // cover variants (heath above, dry meadow below).
    if (characterPalette != 0) {
        return characterPalette;
    }
    if (cover > 0.62f) {
        return 4;
    }
    if (cover < 0.36f) {
        return 5;
    }
    return 0;
}

} // namespace render::terraingen
