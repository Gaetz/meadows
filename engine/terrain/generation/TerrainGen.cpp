#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/GridOps.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"

#include <cmath>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/terrain/Noise.hpp"

namespace render::terraingen {

namespace {

// Seed salts: one per independent noise field, so a control tweak never
// re-rolls an unrelated field.
constexpr u32 kSaltRelief = 0xe5f6a7b8u;
constexpr u32 kSaltReliefWarpX = 0xc3d4e5f6u;
constexpr u32 kSaltReliefWarpZ = 0xd9eafb0cu;
constexpr u32 kSaltHillChain = 0x91c0ffeeu;
constexpr u32 kSaltHardness = 0x11780c1cu;
constexpr u32 kSaltPiece = 0xa1b13e00u;
constexpr u32 kSaltRidgeCol = 0x51d9ec01u;
constexpr u32 kSaltCol = 0xc0110000u;
constexpr u32 kSaltAxialWarp = 0x51deca5eu;
constexpr u32 kSaltBed = 0xbed0bed0u;
constexpr u32 kSaltStoryMask = 0x51ed270bu;
constexpr u32 kSaltStoryRidge = 0xc2b2ae35u;

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

// The PIECES (docs/PAYSAGE.md §7.5): one landmark per jittered cell —
// fbm cannot promise spacing, the grid bounds the distance to the
// nearest piece — drawn at the height of the étage its CENTRE stands
// in (one world sample, only when the point is inside the footprint),
// so a piece never reads a seam of its own. Three silhouettes per
// hash: a dome, a RIDGE modulated along its axis (the saddles are the
// cols, handed to `gentle`), a MESA (flat top, short rim). Pure
// function of (seed, x, z); the analytic mirror gets it for free.
struct PieceSample {
    f32 add { 0.0f };        // meters of base lift
    f32 mesaTop { 0.0f };    // [0,1] on a mesa's flat top
    f32 ridgeFlank { 0.0f }; // [0,1] on a dome/ridge flank
    f32 col { 0.0f };        // [0,1] saddle corridor of a ridge piece
};

PieceSample pieceLayer(const ProceduralControlParams& p, f32 x, f32 z) {
    const RhythmParams& r = p.rhythm;
    PieceSample out;
    const f32 cellSize = r.pieceCellSize;
    const i32 cellX = static_cast<i32>(std::floor(x / cellSize));
    const i32 cellZ = static_cast<i32>(std::floor(z / cellSize));
    // The start cell always has its piece: the meadow the decree
    // flattens gets its landmark (the hill the player orients by).
    const i32 startCellX =
        static_cast<i32>(std::floor(p.world.startX / cellSize));
    const i32 startCellZ =
        static_cast<i32>(std::floor(p.world.startZ / cellSize));
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 gx = cellX + dx;
            const i32 gz = cellZ + dz;
            const auto jitter = [&](u32 k) {
                return noise::lattice((p.seed ^ kSaltPiece) +
                                          k * 0x9e3779b9u,
                                      gx, gz);
            };
            const bool startCell = gx == startCellX && gz == startCellZ;
            if (!startCell && jitter(9) > r.pieceChance) {
                continue;
            }
            const f32 px =
                (static_cast<f32>(gx) + 0.2f + 0.6f * jitter(0)) *
                cellSize;
            const f32 pz =
                (static_cast<f32>(gz) + 0.2f + 0.6f * jitter(1)) *
                cellSize;
            const f32 radius =
                glm::mix(r.pieceRadiusMin, r.pieceRadiusMax, jitter(3));
            const f32 variant = jitter(4);
            const bool mesa = variant >= 0.75f;
            const bool ridge = variant >= 0.4f && !mesa;
            const f32 aspect = ridge ? glm::mix(2.8f, 4.5f, jitter(5))
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
            // The piece's étage: where its centre stands (a piece in
            // the sea does not exist).
            const WorldSample centre = worldSampleAt(p.world, px, pz);
            if (centre.sea) {
                continue;
            }
            const u32 tier = glm::min(
                3u, static_cast<u32>(std::lround(etageIndexFor(
                        p.world, glm::max(centre.base, 0.0f)))));
            const f32 height = glm::mix(r.pieceHeightByEtage[tier][0],
                                        r.pieceHeightByEtage[tier][1],
                                        jitter(2));
            const f32 n = std::sqrt(n2);
            if (mesa) {
                const f32 k = 1.0f - noise::smoothstep01(0.55f, 0.9f, n);
                out.add = glm::max(out.add, height * k);
                out.mesaTop = glm::max(
                    out.mesaTop, 1.0f - noise::smoothstep01(0.3f, 0.55f, n));
                continue;
            }
            // Dome/ridge: C1 kernel the erosion carves into flanks. A
            // ridge is modulated along its length: the lows are
            // saddles — the cols a walker crosses it by.
            const f32 k = (1.0f - n2) * (1.0f - n2);
            f32 mod = 1.0f;
            if (ridge) {
                mod = glm::mix(
                    0.55f, 1.0f,
                    noise::fbm(p.seed ^ kSaltRidgeCol, x, z,
                               1.0f / r.ridgeColWavelength, 2, 2.0f,
                               0.5f));
                out.col = glm::max(out.col, (1.0f - mod) / 0.45f * k);
            }
            out.add = glm::max(out.add, height * k * mod);
            out.ridgeFlank = glm::max(
                out.ridgeFlank,
                noise::smoothstep01(0.08f, 0.4f, k) *
                    (1.0f - noise::smoothstep01(0.6f, 0.9f, k)));
        }
    }
    return out;
}

// Land surface before the coast profile: the FLOOR (the world layer's
// base when the sample carries one, else the tier table's altitude) +
// the tier's warped relief + the piece lift + the massif crests - the
// valley beds + soft strata quantization. Fields at 0 = legacy.
f32 landHeight(const MacroParams& p, u32 seed, const ControlSample& s,
               f32 hillChainWavelength, f32 bedWavelength, f32 x, f32 z) {
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
                                   1.0f / t.reliefWavelength,
                                   glm::max(p.reliefOctaves, 1), 2.0f,
                                   0.5f) *
                            2.0f -
                        1.0f) *
                       t.reliefAmplitude * s.reliefScale;
    const f32 floor = s.hasBase ? p.seaLevel + s.base : t.altitude;
    f32 h = floor + relief + s.plateau;
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
    // Valley beds: a ridged skeleton dug into the floor — the
    // drainage the erosion deepens instead of inventing, fading out
    // near the sea so no bed floods below the waterline.
    if (s.bedDepth > 0.0f && bedWavelength > 1.0f) {
        h -= s.bedDepth *
             noise::ridgedFbm(seed ^ kSaltBed, wx, wz,
                              1.0f / bedWavelength, 2, 2.0f, 0.5f) *
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
        // Two-stage ocean: nearshore ramp, then the luminous coastal
        // PLATEAU, then the talus to the dark open-sea floor. Cliff
        // coasts contract all three bands — calanques plunge.
        const f32 shelf = glm::mix(p.shelfWidth, p.shelfWidth * 0.35f,
                                   cliff);
        const f32 plateauEnd =
            glm::mix(p.shelfEnd, p.shelfEnd * 0.45f, cliff);
        const f32 falloff =
            glm::mix(p.seaFalloff, p.seaFalloff * 0.4f, cliff);
        const f32 shallow =
            glm::mix(waterline, p.seaLevel - p.shallowDepth,
                     noise::smoothstep01(0.0f, shelf, -d));
        // The plateau depth is reached by MID-band and holds flat to
        // plateauEnd — a real shelf, not one long ramp.
        const f32 plateau =
            glm::mix(shallow, p.seaLevel - p.shelfDepth,
                     noise::smoothstep01(shelf, plateauEnd * 0.5f, -d));
        return glm::mix(plateau, p.seaFloor,
                        noise::smoothstep01(plateauEnd, falloff, -d));
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

namespace {

f32 lerpByEtage(const f32 (&table)[4], f32 tier) {
    const f32 t = glm::clamp(tier, 0.0f, 3.0f);
    const u32 i0 = glm::min(static_cast<u32>(t), 2u);
    return glm::mix(table[i0], table[i0 + 1], t - static_cast<f32>(i0));
}

} // namespace

ControlSample ProceduralControls::at(f32 x, f32 z) const {
    WorldSample unused;
    return at(x, z, unused);
}

ControlSample ProceduralControls::at(f32 x, f32 z,
                                     WorldSample& outWorld) const {
    const WorldSample w = worldSampleAt(p.world, x, z);
    outWorld = w;
    const RhythmParams& r = p.rhythm;
    ControlSample s;
    s.sea = w.sea;
    s.base = glm::max(w.base, 0.0f);
    s.hasBase = true;
    s.tier = etageIndexFor(p.world, s.base);
    // The pieces: a landmark's lift, flattened on a mesa top; gated
    // off the beach so no shore rises into a wall.
    const PieceSample piece = pieceLayer(p, x, z);
    const f32 shoreGate = noise::smoothstep01(2.0f, 12.0f, s.base);
    // The story-mode mountains: ridged ranges where a slow mask fires.
    const f32 storyMask = noise::smoothstep01(
        r.storyMountainMaskLow, r.storyMountainMaskHigh,
        noise::fbm(p.seed ^ kSaltStoryMask, x, z,
                   1.0f / r.storyMountainWavelength, 3, 2.0f, 0.5f));
    const f32 storyMountain =
        storyMask * r.storyMountainAmplitude *
        noise::ridgedFbm(p.seed ^ kSaltStoryRidge, x, z,
                         2.0f / r.storyMountainWavelength, 4, 2.0f, 0.5f);
    const f32 lift = glm::max(piece.add, storyMountain);
    s.plateau = lift * shoreGate;
    s.reliefScale = 1.0f - 0.7f * piece.mesaTop;
    // Massif belts: ridged crests sized by the étage, and the uplift
    // that feeds the stream power — never on a mesa top.
    const f32 inland = noise::smoothstep01(25.0f, 80.0f, s.base);
    s.hillRelief =
        w.massif * lerpByEtage(r.crestAmplitudeByEtage, s.tier) * inland;
    s.uplift = w.massif * noise::smoothstep01(0.3f, 0.8f, w.massif) *
               (1.0f - piece.mesaTop);
    // Guaranteed cols: thin stripes of a potential cut across every
    // massif every ~colSpacing — no range is a regional wall. A ridge
    // piece's saddles are corridors too.
    const f32 rangeNeed = noise::smoothstep01(0.35f, 0.7f, w.massif);
    {
        const f32 psi = noise::fbm(p.seed ^ kSaltCol, x, z,
                                   1.0f / (r.colSpacing * 3.2f), 2, 2.0f,
                                   0.5f);
        const f32 colPhase = psi * 3.2f;
        const f32 frac = colPhase - std::floor(colPhase);
        const f32 colK =
            1.0f -
            noise::smoothstep01(0.045f, 0.13f, std::abs(frac - 0.5f));
        s.gentle = glm::clamp(colK * rangeNeed + piece.col, 0.0f, 1.0f);
    }
    // Calm is the RULE: everything that is neither a piece, a massif
    // nor a piece's flank is habitable ground; corridors are members.
    const f32 calm = (1.0f - noise::smoothstep01(20.0f, 70.0f, lift)) *
                     (1.0f - noise::smoothstep01(0.35f, 0.7f, w.massif)) *
                     (1.0f - 0.5f * piece.ridgeFlank);
    s.calm = glm::max(calm, s.gentle);
    // Lithology: a slow hardness field, harder on massif coasts
    // (calanques).
    s.hardness = glm::clamp(
        noise::fbm(p.seed ^ kSaltHardness, x, z, 1.0f / r.hardnessWavelength,
                   3, 2.0f, 0.5f) +
            0.3f * w.massif * w.coast,
        0.0f, 1.0f);
    s.bedDepth = lerpByEtage(r.bedDepthByEtage, s.tier) *
                 noise::smoothstep01(25.0f, 70.0f, s.base);
    s.biome = paletteIdFor(w.temperature, w.moisture, s.base);
    return s;
}

u8 ProceduralControls::biomeIdAt(f32 x, f32 z, f32 tier) const {
    // The climate and the floor come from the world layer — the same
    // sample `at` derives the id from, so the per-texel id and the
    // lattice sample never disagree.
    (void)tier;
    const WorldSample w = worldSampleAt(p.world, x, z);
    return paletteIdFor(w.temperature, w.moisture, glm::max(w.base, 0.0f));
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
    // Control sampling on a COARSE grid, bilinearly interpolated to
    // the sim texels: every control field runs at >= 350 m of
    // wavelength, a 64 m lattice over-samples all of them (5+ samples
    // per wave) while the full ControlSource::at — carriers, layout
    // kernels, landmark grids, valley potential — is by far the
    // dominant stage-1 cost at 16 m. The sea boolean takes the nearest
    // lattice point; the biome id is re-evaluated per texel through the
    // cheap biomeIdAt seam (a nearest id drew 64 m axis-aligned biome
    // stairs); the analytic mirror stays exact-pointwise (the
    // ~1 m interpolation residue is folded into the erosion-fit
    // calibration and hidden by the rim blend).
    const u32 step = spec.texelSize < 60.0f
                         ? glm::max(1u, static_cast<u32>(std::lround(
                                            64.0f / spec.texelSize)))
                         : 1u;
    const u32 coarseN = (spec.n + step - 1) / step + 1;
    vector<ControlSample> coarse(static_cast<size_t>(coarseN) * coarseN);
    for (u32 row = 0; row < coarseN; ++row) {
        for (u32 col = 0; col < coarseN; ++col) {
            coarse[static_cast<size_t>(row) * coarseN + col] =
                controls.at(
                    spec.x(glm::min(col * step, spec.n - 1)),
                    spec.z(glm::min(row * step, spec.n - 1)));
        }
    }
    const auto lerpSample = [&](u32 col, u32 row) {
        if (step == 1) {
            return coarse[static_cast<size_t>(row) * coarseN + col];
        }
        const u32 c0 = col / step;
        const u32 r0 = row / step;
        const u32 c1 = glm::min(c0 + 1, coarseN - 1);
        const u32 r1 = glm::min(r0 + 1, coarseN - 1);
        const f32 tc =
            static_cast<f32>(col - c0 * step) / static_cast<f32>(step);
        const f32 tr =
            static_cast<f32>(row - r0 * step) / static_cast<f32>(step);
        const ControlSample& s00 =
            coarse[static_cast<size_t>(r0) * coarseN + c0];
        const ControlSample& s10 =
            coarse[static_cast<size_t>(r0) * coarseN + c1];
        const ControlSample& s01 =
            coarse[static_cast<size_t>(r1) * coarseN + c0];
        const ControlSample& s11 =
            coarse[static_cast<size_t>(r1) * coarseN + c1];
        const auto lerp = [&](f32 a, f32 b, f32 c, f32 d) {
            return glm::mix(glm::mix(a, b, tc), glm::mix(c, d, tc), tr);
        };
        ControlSample out;
        out.tier = lerp(s00.tier, s10.tier, s01.tier, s11.tier);
        out.uplift =
            lerp(s00.uplift, s10.uplift, s01.uplift, s11.uplift);
        out.plateau =
            lerp(s00.plateau, s10.plateau, s01.plateau, s11.plateau);
        out.hillRelief = lerp(s00.hillRelief, s10.hillRelief,
                              s01.hillRelief, s11.hillRelief);
        out.gentle =
            lerp(s00.gentle, s10.gentle, s01.gentle, s11.gentle);
        out.calm = lerp(s00.calm, s10.calm, s01.calm, s11.calm);
        out.hardness = lerp(s00.hardness, s10.hardness, s01.hardness,
                            s11.hardness);
        out.reliefScale = lerp(s00.reliefScale, s10.reliefScale,
                               s01.reliefScale, s11.reliefScale);
        out.trunk = lerp(s00.trunk, s10.trunk, s01.trunk, s11.trunk);
        out.trunkDepth = lerp(s00.trunkDepth, s10.trunkDepth,
                              s01.trunkDepth, s11.trunkDepth);
        out.axisCos = lerp(s00.axisCos, s10.axisCos, s01.axisCos,
                           s11.axisCos);
        out.axisSin = lerp(s00.axisSin, s10.axisSin, s01.axisSin,
                           s11.axisSin);
        out.axisStrength =
            lerp(s00.axisStrength, s10.axisStrength, s01.axisStrength,
                 s11.axisStrength);
        out.base = lerp(s00.base, s10.base, s01.base, s11.base);
        out.bedDepth = lerp(s00.bedDepth, s10.bedDepth, s01.bedDepth,
                            s11.bedDepth);
        const ControlSample& nearest =
            coarse[static_cast<size_t>(tr < 0.5f ? r0 : r1) * coarseN +
                   (tc < 0.5f ? c0 : c1)];
        out.sea = nearest.sea;
        out.hasBase = nearest.hasBase;
        return out;
    };
    for (u32 row = 0; row < spec.n; ++row) {
        for (u32 col = 0; col < spec.n; ++col) {
            const size_t i = static_cast<size_t>(row) * spec.n + col;
            ControlSample s = lerpSample(col, row);
            // Biome id PER TEXEL (nearest-sampling an id on the coarse
            // lattice drew 64 m axis-aligned biome stairs); the tier the
            // alpine rule needs interpolates fine.
            if (step > 1) {
                s.biome = controls.biomeIdAt(spec.x(col), spec.z(row),
                                             s.tier);
            }
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
                           params.hillChainWavelength, params.bedWavelength,
                           spec.x(col), spec.z(row)));
            out.height[i] =
                coastProfile(params, land, samples[i].tier,
                             out.seaDist[i], samples[i].hardness);
        }
    }
    return out;
}

namespace {

constexpr u32 kSaltBorderStyle = 0xb02de125u;
// Meters of shore per continent unit per wavelength (the carrier's
// typical gradient after contrast): hand-tuned against the bake's
// distance field.
constexpr f32 kShoreProxy = 0.5f;
constexpr u32 kSaltBorderCrest = 0xc2e57000u;
constexpr u32 kSaltBorderIsle = 0x151e7000u;
constexpr u32 kSaltBorderWander = 0x3a2de300u;

// One border line\'s contribution factors at |d| meters from it.
f32 mountainProfile(f32 dist) {
    const f32 t =
        1.0f - glm::clamp(dist / kMapBorderMountainHalf, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t); // smooth rise, crest on the line
}

f32 seaProfile(f32 dist) {
    // Flat channel near the line, coasts descending over the half.
    const f32 t =
        1.0f - glm::clamp((dist - 200.0f) / (kMapBorderSeaHalf - 200.0f),
                          0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct BorderSample {
    f32 mountain { 0.0f }; // lift factor (crest variation applied)
    f32 mountainRaw { 0.0f }; // profile alone (erosion keep)
    f32 sea { 0.0f };
    f32 island { 0.0f };
};

// The two nearest border lines (one vertical, one horizontal) decide
// everything: mapSize (24+ km) dwarfs every band, farther lines never
// contribute.
BorderSample sampleBorders(const ProceduralControls& controls,
                           const MacroParams& macroParams,
                           const MapGridSpec& g, f32 x, f32 z) {
    BorderSample out;
    const auto line = [&](bool vertical) {
        const f32 along = vertical ? z : x;
        const f32 across = vertical ? x : z;
        const i32 lineIndex =
            static_cast<i32>(std::floor(across / g.mapSize + 0.5f));
        const u32 lineSeed = g.seed ^
                             (vertical ? 0x9e3779b9u : 0x85ebca6bu) ^
                             static_cast<u32>(lineIndex) * 0x27d4eb2fu;
        // The line MEANDERS: a long-range wave along the line offsets
        // its position — coasts and ranges wander naturally, and both
        // sides share the same pure warp.
        const f32 wander =
            (noise::fbm(lineSeed ^ kSaltBorderWander, along, 0.0f,
                        1.0f / kMapBorderWanderWavelength, 3, 2.0f,
                        0.5f) -
             0.5f) *
            2.0f * kMapBorderWander;
        const f32 dist = std::abs(
            across - (static_cast<f32>(lineIndex) * g.mapSize + wander));
        // Style is hashed per SEGMENT (line x crossed cell); near a
        // segment junction the two styles CROSS-FADE along the line —
        // a sea arm closes into a bay while the range rises out of it,
        // instead of a channel stopping dead at the corner.
        const f32 cellF = along / g.mapSize;
        const i32 cellCross = static_cast<i32>(std::floor(cellF));
        const f32 local =
            (cellF - static_cast<f32>(cellCross)) * g.mapSize;
        const auto ridges = [&](i32 cell) {
            return mapBorderStyleResolved(controls, macroParams, g,
                                          lineIndex, cell, vertical) ==
                           MapEdgeStyle::Ridges
                       ? 1.0f
                       : 0.0f;
        };
        f32 r = ridges(cellCross);
        if (local < kMapBorderStyleBlend) {
            const f32 t = 0.5f + 0.5f * noise::smoothstep01(
                                            0.0f, 1.0f,
                                            local / kMapBorderStyleBlend);
            r = glm::mix(ridges(cellCross - 1), r, t);
        } else if (local > g.mapSize - kMapBorderStyleBlend) {
            const f32 t =
                0.5f + 0.5f * noise::smoothstep01(
                                  0.0f, 1.0f,
                                  (g.mapSize - local) /
                                      kMapBorderStyleBlend);
            r = glm::mix(ridges(cellCross + 1), r, t);
        }
        if (r > 0.0f) {
            const f32 p = mountainProfile(dist) * r;
            if (p > 0.0f) {
                // Crest height varies ALONG the line: peaks and
                // saddles — the natural cols. [0.3, 1] of the lift.
                const f32 var = noise::fbm(
                    lineSeed ^ kSaltBorderCrest, along, 0.0f,
                    1.0f / kMapBorderCrestWavelength, 2, 2.0f, 0.5f);
                const f32 varied = p * glm::mix(0.3f, 1.0f, var);
                out.mountain = glm::max(out.mountain, varied);
                // The erosion keep follows the VARIED crest, not the
                // raw profile: saddles resist less, so the fastscape
                // carves them into WALKABLE passes — peaks keep their
                // wall (a full-strength keep at the cols left them
                // too steep to climb).
                out.mountainRaw = glm::max(out.mountainRaw, varied);
            }
        }
        if (r < 1.0f) {
            const f32 p = seaProfile(dist) * (1.0f - r);
            if (p > 0.0f) {
                out.sea = glm::max(out.sea, p);
                // Occasional islets mid-channel: land appearing
                // progressively inside the sea arm.
                const f32 isle =
                    noise::fbm(lineSeed ^ kSaltBorderIsle, along, 0.0f,
                               1.0f / 2200.0f, 2, 2.0f, 0.5f);
                const f32 centered =
                    1.0f - glm::clamp(dist / 700.0f, 0.0f, 1.0f);
                out.island = glm::max(
                    out.island,
                    noise::smoothstep01(0.68f, 0.8f, isle) *
                        (1.0f - r) * centered * centered *
                        (3.0f - 2.0f * centered));
            }
        }
    };
    line(true);
    line(false);
    return out;
}

} // namespace

MapEdgeStyle mapBorderStyle(u32 seed, i32 lineIndex, i32 cellCross,
                            bool vertical) {
    u64 h = 14695981039346656037ull;
    const auto mix = [&h](u64 v) {
        for (int byte = 0; byte < 8; ++byte) {
            h ^= (v >> (byte * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    mix(seed ^ kSaltBorderStyle);
    mix(static_cast<u64>(static_cast<u32>(lineIndex)));
    mix(static_cast<u64>(static_cast<u32>(cellCross)));
    mix(vertical ? 0x76ull : 0x68ull);
    return (h & 1ull) != 0ull ? MapEdgeStyle::Ridges
                              : MapEdgeStyle::Sea;
}

MapEdgeStyle mapBorderStyleResolved(const ProceduralControls& controls,
                                    const MacroParams& macro,
                                    const MapGridSpec& spec,
                                    i32 lineIndex, i32 cellCross,
                                    bool vertical) {
    const MapEdgeStyle proposed =
        mapBorderStyle(spec.seed, lineIndex, cellCross, vertical);
    if (proposed != MapEdgeStyle::Sea) {
        return proposed;
    }
    // Sea veto: a sea arm only stands where the analytic world already
    // reads coastal along the segment — deep inland it demotes to the
    // canonical land-land border (Ridges). Memoized per segment (the
    // analytic samples are the cost); thread-local keeps it pure.
    thread_local std::unordered_map<u64, MapEdgeStyle> memo;
    u64 key = static_cast<u64>(static_cast<u32>(lineIndex)) |
              (static_cast<u64>(static_cast<u32>(cellCross)) << 32);
    key ^= vertical ? 0x9e3779b97f4a7c15ull : 0xc2b2ae3d27d4eb4full;
    key ^= static_cast<u64>(spec.seed) * 0x100000001b3ull;
    key ^= static_cast<u64>(
               static_cast<i64>(spec.seaLevel * 64.0f)) << 17;
    key ^= static_cast<u64>(spec.mapSize) << 3;
    if (const auto it = memo.find(key); it != memo.end()) {
        return it->second;
    }
    constexpr u32 kSamples = 9;
    u32 oceanish = 0;
    for (u32 i = 0; i < kSamples; ++i) {
        const f32 along =
            (static_cast<f32>(cellCross) +
             (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kSamples)) *
            spec.mapSize;
        const f32 lineAt = static_cast<f32>(lineIndex) * spec.mapSize;
        const f32 sx = vertical ? lineAt : along;
        const f32 sz = vertical ? along : lineAt;
        if (macroHeightAnalytic(controls, macro, sx, sz) <
            spec.seaLevel + 2.0f) {
            ++oceanish;
        }
    }
    const MapEdgeStyle resolved =
        static_cast<f32>(oceanish) <
                kMapBorderSeaVetoOceanFrac * static_cast<f32>(kSamples)
            ? MapEdgeStyle::Ridges
            : MapEdgeStyle::Sea;
    memo.emplace(key, resolved);
    return resolved;
}

f32 applyMapGridShape(const ProceduralControls& controls,
                      const MacroParams& macro, const MapGridSpec& spec,
                      f32 x, f32 z, f32 h) {
    if (!spec.valid) {
        return h;
    }
    const BorderSample sample = sampleBorders(controls, macro, spec, x, z);
    // Coherence gate (the proximity rule): the transition reads the
    // ground under it — border features belong to the LAND the lattice
    // separates, never to the open ocean the line happens to cross.
    const f32 land = noise::smoothstep01(
        spec.seaLevel + kMapBorderLandFadeLow,
        spec.seaLevel + kMapBorderLandFadeHigh, h);
    // The range rises progressively out of the EXISTING terrain (an
    // additive lift, never a wall out of the ground) — and only out of
    // LAND: the chain tapers into the coast instead of marching across
    // the sea.
    h += kMapBorderMountainLift * sample.mountain * land;
    // A sea arm drowns the land it crosses (coastal cliffs where a
    // range dives in); islets are drowned land resisting.
    if (sample.sea > 0.0f) {
        const f32 island = glm::clamp(sample.island, 0.0f, 1.0f) * land;
        const f32 drown = sample.sea * (1.0f - island);
        // min(): the arm only DEEPENS — an already-deeper ocean floor
        // stays, instead of being lifted into a shallow shelf.
        h = glm::min(
            h, glm::mix(h, spec.seaLevel - kMapBorderSeaDepth, drown));
        // An islet stands clear of the water even where the base
        // channel would be deep.
        if (island > 0.0f) {
            h = glm::max(h,
                         glm::mix(spec.seaLevel - kMapBorderSeaDepth,
                                  spec.seaLevel + 26.0f, island));
        }
    }
    return h;
}

f32 mapGridRidgeFactor(const ProceduralControls& controls,
                       const MacroParams& macro, const MapGridSpec& spec,
                       f32 x, f32 z, f32 h) {
    if (!spec.valid) {
        return 0.0f;
    }
    const f32 land = noise::smoothstep01(
        spec.seaLevel + kMapBorderLandFadeLow,
        spec.seaLevel + kMapBorderLandFadeHigh, h);
    return sampleBorders(controls, macro, spec, x, z).mountainRaw * land;
}

f32 macroHeightAnalytic(const ProceduralControls& controls,
                        const MacroParams& params, f32 x, f32 z) {
    // The sample's own world sample serves the shore distance below:
    // one evaluation instead of two.
    WorldSample w;
    const ControlSample s = controls.at(x, z, w);
    const f32 land = recurveLand(
        params, landHeight(params, controls.params().seed, s,
                           controls.params().rhythm.crestWavelength,
                           controls.params().rhythm.bedWavelength, x, z));
    // Shore distance approximated from the continent value: the
    // carrier's typical slope turns continent units into meters (good
    // enough for silhouettes and boundary conditions; the bake's grid
    // distance field is the truth inside a map).
    const WorldLayerParams& world = controls.params().world;
    const f32 d = (w.continent - world.seaThreshold) *
                  world.continentWavelength * kShoreProxy;
    return coastProfile(params, land, s.tier, d, s.hardness);
}

} // namespace render::terraingen
