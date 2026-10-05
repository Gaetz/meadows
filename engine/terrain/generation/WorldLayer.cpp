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
constexpr u32 kSaltTemperature = 0x7ea7be57u;
constexpr u32 kSaltMoisture = 0x6d015745u;
constexpr u32 kSaltTemperatureSlow = 0x7ea7be58u;
constexpr u32 kSaltMoistureSlow = 0x6d015746u;

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
    // Start decree: land, low, temperate around the start.
    const f32 dStart = std::hypot(x - p.startX, z - p.startZ);
    const f32 pull =
        1.0f - noise::smoothstep01(p.startRadius,
                                   p.startRadius + p.startFade, dStart);
    c = glm::mix(c, glm::max(c, p.seaThreshold + 0.14f), pull);
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
    // The étage at the warped point (provinces wander with the coasts).
    f32 e = noise::fbm(p.seed ^ kSaltEtage, wx, wz, 1.0f / p.etageWavelength,
                       3, 2.0f, 0.45f);
    e = 0.5f + (e - 0.5f) * 1.3f;
    w.etage = glm::clamp(e, 0.0f, 1.0f);
    // Massif belts: ridges of a slow field, only on the higher
    // provinces, never at the start.
    const f32 massifRaw = noise::fbm(p.seed ^ kSaltMassif, wx, wz,
                                     1.0f / p.massifWavelength, 3, 2.0f,
                                     0.5f);
    w.massif = noise::smoothstep01(0.50f, 0.74f, massifRaw) *
               noise::smoothstep01(0.45f, 0.62f, w.etage) * (1.0f - pull);
    // The floor: province altitude plus the massif lift, ramped up
    // from the carrier's shoreline (a wider ramp under high provinces
    // keeps the coastal escarpment walkable; land the coast detail
    // adds past the carrier's shore stays low); sea floors sink with
    // the carrier.
    if (w.sea) {
        w.base = -70.0f * noise::smoothstep01(0.0f, 0.25f, p.seaThreshold - c);
    } else {
        f32 alt = provinceAltitude(w.etage) + p.massifLift * w.massif;
        // The start decree on the FLOOR itself (not on the province
        // field, whose table is steep up high): low, keeping a trace
        // of its own variation (the tier relief and the beds give the
        // rivers their texture).
        alt = glm::mix(alt, glm::min(80.0f, 40.0f + (alt - 40.0f) * 0.06f),
                       pull);
        const f32 inland = noise::smoothstep01(
            0.0f, p.coastBand * (1.5f + 4.0f * w.etage),
            carrier - p.seaThreshold);
        w.base = alt * inland;
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
    temperature = glm::mix(temperature, 0.50f, pull);
    moisture = glm::mix(moisture, 0.55f, pull);
    w.temperature = temperature;
    w.moisture = moisture;
    return w;
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

u8 paletteIdFor(f32 temperature, f32 moisture, f32 base) {
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
    return 0;
}

} // namespace render::terraingen
