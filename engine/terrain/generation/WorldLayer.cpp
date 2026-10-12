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
constexpr u32 kSaltContinentCell = 0xa2c41ce1u;
constexpr u32 kSaltContinentShape = 0x5ba9e002u;
constexpr u32 kSaltCoastWarpX = 0xc0a37a91u;
constexpr u32 kSaltCoastWarpZ = 0xc0a37a92u;

// The edge guard of the archipelago lattice: land dies this far inside
// the host core (the coast detail may push it back up to ~350 m, the
// guarantee keeps ~290 m of margin). The corner radius keeps the
// bound from ever printing a square corner into a coastline (dev
// report: the start continent wore one).
constexpr f32 kContinentEdgeMargin = 640.0f;
constexpr f32 kContinentEdgeFalloff = 2048.0f;
constexpr f32 kContinentCornerRadius = 3072.0f;

u32 continentHash(const WorldLayerParams& p, i32 i, i32 j) {
    return (p.seed ^ kSaltContinentCell) ^
           (static_cast<u32>(i) * 0x9e3779b9u) ^
           (static_cast<u32>(j) * 0x85ebca6bu);
}

struct ContinentCell {
    bool present { false };
    f32 coreX0 { 0.0f }; // host core rect
    f32 coreZ0 { 0.0f };
    f32 cx { 0.0f }; // jittered heart
    f32 cz { 0.0f };
    f32 radius { 0.0f };
};

// Lattice cell (i, j) of the archipelago: host super-cell staggered by
// half a pitch on odd rows (the grid stops reading as a grid), a
// presence roll (some cells are open ocean), a per-continent size.
// Cell (0, 0) is the start's: always present, full size, its heart
// forced toward the start.
ContinentCell continentCellAt(const WorldLayerParams& p, i32 i, i32 j) {
    ContinentCell cell;
    const f32 superSize = p.continentSuperSize;
    const f32 pitch =
        superSize * static_cast<f32>(p.continentCellSupers);
    const u32 hash = continentHash(p, i, j);
    const bool start = i == 0 && j == 0;
    if (!start && static_cast<f32>((hash >> 8) & 0xffu) *
                          (1.0f / 255.0f) >
                      p.continentChance) {
        return cell; // this cell is ocean
    }
    cell.present = true;
    const f32 stagger =
        (j & 1) != 0 ? superSize *
                           static_cast<f32>(p.continentCellSupers / 2u)
                     : 0.0f;
    cell.coreX0 =
        p.continentLatticeOrigin + static_cast<f32>(i) * pitch + stagger;
    cell.coreZ0 = p.continentLatticeOrigin + static_cast<f32>(j) * pitch;
    const f32 sizeRoll =
        static_cast<f32>((hash >> 16) & 0xffu) * (1.0f / 255.0f);
    cell.radius =
        start ? p.continentRadius
              : p.continentRadius * glm::mix(0.55f, 1.0f, sizeRoll);
    // The whole slack of the core is the jitter room: small continents
    // wander kilometers, the big ones stay nearer their heart. The
    // start continent does not wander at all — the lattice origin
    // already centers its core on the start country.
    const f32 room =
        superSize * 0.5f - kContinentEdgeMargin -
        (1.0f + p.continentShapeAmp) * cell.radius;
    const f32 jit =
        start ? 0.0f : glm::max(p.continentJitter, 0.7f * room);
    const f32 jx = (static_cast<f32>(hash & 0xffu) * (1.0f / 255.0f) *
                        2.0f -
                    1.0f) *
                   jit;
    const f32 jz = (static_cast<f32>((hash >> 24) & 0xffu) *
                        (1.0f / 255.0f) * 2.0f -
                    1.0f) *
                   jit;
    cell.cx = cell.coreX0 + superSize * 0.5f + jx;
    cell.cz = cell.coreZ0 + superSize * 0.5f + jz;
    return cell;
}

// Inside distance (m, negative outside) to a host core shrunk by the
// edge margin, with ROUNDED corners — the standard rounded-rect SDF.
f32 roundedCoreInside(const WorldLayerParams& p, const ContinentCell& c,
                      f32 x, f32 z) {
    const f32 half = p.continentSuperSize * 0.5f;
    const f32 qx = std::abs(x - (c.coreX0 + half)) -
                   (half - kContinentEdgeMargin -
                    kContinentCornerRadius);
    const f32 qz = std::abs(z - (c.coreZ0 + half)) -
                   (half - kContinentEdgeMargin -
                    kContinentCornerRadius);
    const f32 ox = glm::max(qx, 0.0f);
    const f32 oz = glm::max(qz, 0.0f);
    const f32 outside = std::sqrt(ox * ox + oz * oz) +
                        glm::min(glm::max(qx, qz), 0.0f) -
                        kContinentCornerRadius;
    return -outside;
}

// Continent field in v units: a STRONGLY domain-warped disc — the
// coastline is the isoline of a radius modulated by a per-continent
// rim fbm, read through a ±coastWarp deformation of the whole plane
// (the august continentLayout's recipe at the right scale: deep
// gulfs, peninsulas, no readable circle). The 45 km carrier cannot
// shape a 20 km continent (it is all-high or all-low across one
// cell — measured: the coast fell back to the envelope and read as a
// disc). Geometry warped, guard NOT: the rounded core SDF stays on
// raw coordinates, so the river-head ownership invariant holds.
constexpr f32 kContinentCoastWarp = 4200.0f;
constexpr f32 kContinentCoastWarpWavelength = 9000.0f;

f32 latticeBlob(const WorldLayerParams& p, f32 x, f32 z) {
    const f32 pitch =
        p.continentSuperSize * static_cast<f32>(p.continentCellSupers);
    const i32 i0 = static_cast<i32>(
        std::floor((x - p.continentLatticeOrigin) / pitch));
    const i32 j0 = static_cast<i32>(
        std::floor((z - p.continentLatticeOrigin) / pitch));
    f32 best = -8.0f;
    f32 wx3 = 0.0f;
    f32 wz3 = 0.0f;
    bool warped = false;
    for (i32 j = j0 - 1; j <= j0 + 1; ++j) {
        for (i32 i = i0 - 2; i <= i0 + 1; ++i) { // stagger reaches west
            const ContinentCell cell = continentCellAt(p, i, j);
            if (!cell.present) {
                continue;
            }
            if (std::hypot(x - cell.cx, z - cell.cz) > pitch) {
                continue; // saturated sea either way, spare the fbm
            }
            if (!warped) {
                // One strong warp serves every candidate: global and
                // continuous, each contribution stays seam-free.
                wx3 = x + (noise::fbm(p.seed ^ kSaltCoastWarpX, x, z,
                                      1.0f /
                                          kContinentCoastWarpWavelength,
                                      4, 2.0f, 0.5f) *
                               2.0f -
                           1.0f) *
                              kContinentCoastWarp;
                wz3 = z + (noise::fbm(p.seed ^ kSaltCoastWarpZ, x, z,
                                      1.0f /
                                          kContinentCoastWarpWavelength,
                                      4, 2.0f, 0.5f) *
                               2.0f -
                           1.0f) *
                              kContinentCoastWarp;
                warped = true;
            }
            const u32 hash = continentHash(p, i, j);
            const f32 d = std::hypot(wx3 - cell.cx, wz3 - cell.cz);
            f32 rim = noise::fbm(hash ^ kSaltContinentShape, wx3, wz3,
                                 1.0f / p.continentShapeWavelength, 4,
                                 2.0f, 0.5f) *
                          2.0f -
                      1.0f;
            if (i == 0 && j == 0) {
                // The start continent keeps a mild floor on its rim:
                // the lattice origin centers its core on the start,
                // deep bites elsewhere are welcome.
                rim = glm::max(rim, -0.3f);
            }
            const f32 rEff =
                cell.radius * (1.0f + p.continentShapeAmp * rim);
            const f32 v =
                (rEff - d) /
                glm::max(kContinentCoastSlope * cell.radius, 1.0f);
            const f32 core =
                roundedCoreInside(p, cell, x, z) / kContinentEdgeFalloff;
            best = glm::max(best, glm::min(v, core));
        }
    }
    return best;
}

// Signed guard field, in v units: >= 0 only inside a PRESENT host
// core (rounded, margin deep) — what the start pull may not override.
f32 latticeGuard(const WorldLayerParams& p, f32 x, f32 z) {
    const f32 pitch =
        p.continentSuperSize * static_cast<f32>(p.continentCellSupers);
    const i32 i0 = static_cast<i32>(
        std::floor((x - p.continentLatticeOrigin) / pitch));
    const i32 j0 = static_cast<i32>(
        std::floor((z - p.continentLatticeOrigin) / pitch));
    f32 best = -8.0f;
    for (i32 j = j0 - 1; j <= j0 + 1; ++j) {
        for (i32 i = i0 - 2; i <= i0 + 1; ++i) {
            const ContinentCell cell = continentCellAt(p, i, j);
            if (!cell.present) {
                continue;
            }
            best = glm::max(best, roundedCoreInside(p, cell, x, z) /
                                      kContinentEdgeFalloff);
        }
    }
    return best;
}

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
    const bool lattice = p.continentCellSupers > 0;
    f32 c;
    if (lattice) {
        c = p.seaThreshold + kContinentGain * latticeBlob(p, x, z);
    } else {
        c = noise::fbm(p.seed ^ kSaltContinent, wx, wz,
                       1.0f / p.continentWavelength, 3, 2.0f, 0.5f);
        // Contrast: raw fbm hugs its midline; stretched so land and
        // sea are decided and the coast belt stays a belt.
        c = 0.5f + (c - 0.5f) * 1.6f;
    }
    // Legacy meadow pull (0 = off, the default): the start is the
    // world as drawn — land there is the lattice clamp's job.
    const f32 dStart = std::hypot(x - p.startX, z - p.startZ);
    const f32 pull =
        1.0f - noise::smoothstep01(p.startRadius,
                                   p.startRadius + p.startFade, dStart);
    if (lattice) {
        // The edge guard wins over everything, start pull included:
        // land past a host core would hand a river head to a window
        // that cannot trace it.
        c = glm::min(c, p.seaThreshold +
                            kContinentGain * latticeGuard(p, x, z));
    }
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
    // provinces.
    const f32 massifRaw = noise::fbm(p.seed ^ kSaltMassif, wx, wz,
                                     1.0f / p.massifWavelength, 3, 2.0f,
                                     0.5f);
    w.massif = noise::smoothstep01(0.50f, 0.74f, massifRaw) *
               noise::smoothstep01(0.45f, 0.62f, w.etage);
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
    // coasts.
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
    u(p.continentCellSupers);
    f(p.continentSuperSize);
    f(p.continentLatticeOrigin);
    f(p.continentRadius);
    f(p.continentJitter);
    f(p.continentShapeWavelength);
    f(p.continentShapeAmp);
    f(p.continentChance);
    u(p.borderFromWorld ? 1u : 0u);
    f(p.borderMassifMin);
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
