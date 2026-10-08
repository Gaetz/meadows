#include "engine/terrain/generation/ZonePlan.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/Hash.hpp"
#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/MapGrid.hpp"

namespace render::terraingen {

namespace {

constexpr u32 kSaltZone = 0x20e5a1f3u;
constexpr u32 kSaltZoneWarpX = 0x7a9e1c35u;
constexpr u32 kSaltZoneWarpZ = 0x3c1d9e7bu;
constexpr u32 kSaltTrend = 0x5e1ec7d1u;

f32 roll01(u32 hash, u32 k) {
    return static_cast<f32>(core::hashU32(hash ^ (k * 0x9e3779b9u)) &
                            0xffffffu) /
           16777216.0f;
}

struct Zone {
    f32 x { 0.0f };
    f32 z { 0.0f };
    u32 hash { 0 };
    u32 archetype { 0 };
    i32 storey { 0 };
    f32 storeyHeight { 0.0f }; // storey x step x start gate
    f32 pieceScale { 1.0f };   // hashed size variation of the piece
};

struct Memo {
    u64 key { 0 };
    bool valid { false };
    std::unordered_map<u64, Zone> cells;
};

// The params key: the full hash walks ~400 fields (the archetype
// table included), a per-call cost the per-texel synthesis cannot pay.
// A cheap FINGERPRINT (every scalar, the rows' key numbers) decides
// when to re-hash — never a pointer identity, which a stack temporary
// reusing an address would fool.
u64 paramsKey(const WorldLayerParams& world, const ZoneParams& zones) {
    u64 fp = 1469598103934665603ull;
    const auto mix = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            fp ^= bytes[i];
            fp *= 1099511628211ull;
        }
    };
    const auto f = [&](f32 v) { mix(&v, sizeof(v)); };
    mix(&world.seed, sizeof(world.seed));
    f(world.startX);
    f(world.startZ);
    f(world.startRadius);
    f(world.etageWavelength);
    f(world.continentWavelength);
    f(world.massifWavelength);
    f(world.climateWavelength);
    f(zones.cellSize);
    f(zones.jitter);
    f(zones.borderWarp);
    f(zones.borderWarpWavelength);
    f(zones.stepHeight);
    mix(&zones.storeys, sizeof(zones.storeys));
    f(zones.trendWavelength);
    f(zones.trendContrast);
    f(zones.wallWidthOne);
    f(zones.wallWidthHigh);
    f(zones.rampWidth);
    f(zones.startGap);
    f(zones.mapSize);
    mix(&zones.rampartSteps, sizeof(zones.rampartSteps));
    for (const ZoneArchetype& a : zones.archetypes) {
        f(a.weight);
        mix(&a.storeyBias, sizeof(a.storeyBias));
        f(a.reliefMul);
        f(a.terrace);
        f(a.cliffStep);
        f(a.minEtage);
        f(a.maxEtage);
        mix(&a.piece, sizeof(a.piece));
    }
    struct Last {
        u64 fingerprint { 0 };
        u64 key { 0 };
        bool valid { false };
    };
    thread_local Last last;
    if (!last.valid || last.fingerprint != fp) {
        last.fingerprint = fp;
        last.key = hashParams(world) ^ (hashParams(zones) * 0x9e3779b97f4a7c15ull);
        last.valid = true;
    }
    return last.key;
}

Memo& memoFor(const WorldLayerParams& world, const ZoneParams& zones) {
    thread_local Memo memo;
    const u64 key = paramsKey(world, zones);
    if (!memo.valid || memo.key != key || memo.cells.size() > 65536) {
        memo.cells.clear();
        memo.key = key;
        memo.valid = true;
    }
    return memo;
}

u64 cellKey(i32 cx, i32 cz) {
    return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
           static_cast<u64>(static_cast<u32>(cz));
}

f32 trendAt(const WorldLayerParams& world, const ZoneParams& zones, f32 x,
            f32 z) {
    const f32 t = noise::fbm(world.seed ^ kSaltTrend, x, z,
                             1.0f / zones.trendWavelength, 3, 2.0f, 0.5f);
    return glm::clamp(0.5f + (t - 0.5f) * zones.trendContrast, 0.0f, 1.0f);
}

// A cell's own draw: site, archetype and RAW storey (the trend rounded
// plus the archetype's bias), pure — the bowl rule below reads the
// neighbours' raw storeys without touching the memo.
struct RawCell {
    f32 x, z;
    u32 hash;
    u32 archetype;
    i32 storey;
    bool sea;
    bool startMeadow;
    f32 dStart;
};

bool holdsWater(const ZoneArchetype& a) {
    return a.storeyBias < 0 || a.wetBias >= 0.4f;
}

RawCell rawCell(const WorldLayerParams& world, const ZoneParams& zones, i32 cx,
                i32 cz) {
    RawCell c;
    c.hash = core::hashU32(world.seed ^ kSaltZone ^
                           core::hashU32(static_cast<u32>(cx) ^
                                         (static_cast<u32>(cz) * 0x85ebca6bu)));
    // The lattice is anchored HALF A CELL off the world origin: a map
    // line (a multiple of 8192 = 16 cells of 512) must never coincide
    // with a cell row, or the walls between rows run along the border
    // and the master drainage follows their foot (dev bug report
    // 2026-10-08: a river along the map line).
    c.x = (static_cast<f32>(cx) + 1.0f + (roll01(c.hash, 1) - 0.5f) * zones.jitter) *
          zones.cellSize;
    c.z = (static_cast<f32>(cz) + 1.0f + (roll01(c.hash, 2) - 0.5f) * zones.jitter) *
          zones.cellSize;
    const WorldSample w = worldSampleAt(world, c.x, c.z);
    c.dStart = std::hypot(c.x - world.startX, c.z - world.startZ);
    const vector<ZoneArchetype>& table = zoneArchetypesOf(zones);
    c.sea = w.sea;
    c.startMeadow = c.dStart < world.startRadius + zones.startGap;
    c.archetype = 0;
    c.storey = 0;
    if (c.sea || c.startMeadow || table.empty()) {
        return c;
    }
    const auto fits = [&](const ZoneArchetype& a) {
        return w.etage >= a.minEtage && w.etage <= a.maxEtage &&
               w.massif >= a.minMassif && w.massif <= a.maxMassif &&
               w.coast >= a.minCoast && w.coast <= a.maxCoast &&
               w.moisture >= a.minMoisture && w.moisture <= a.maxMoisture;
    };
    f32 total = 0.0f;
    for (const ZoneArchetype& a : table) {
        total += fits(a) ? glm::max(a.weight, 0.0f) : 0.0f;
    }
    if (total > 0.0f) {
        f32 pick = roll01(c.hash, 4) * total;
        for (u32 i = 0; i < table.size(); ++i) {
            if (!fits(table[i])) {
                continue;
            }
            pick -= glm::max(table[i].weight, 0.0f);
            if (pick <= 0.0f) {
                c.archetype = i;
                break;
            }
        }
    }
    const f32 trend = trendAt(world, zones, c.x, c.z);
    const i32 raw = static_cast<i32>(
        std::lround(trend * static_cast<f32>(zones.storeys)));
    c.storey = glm::clamp(raw + table[c.archetype].storeyBias, 0,
                          static_cast<i32>(zones.storeys));
    return c;
}

// Does a Ridges map line cross this cell's row or column? Read on the
// NOMINAL (unjittered) site: the lattice sits a half cell off the
// origin (rawCell), so a line falls on a site row — one row of cells
// straddles it when cellSize divides mapSize.
bool ridgeLineCrossesCell(const WorldLayerParams& world,
                          const ZoneParams& zones, i32 cx, i32 cz) {
    const f32 sx = (static_cast<f32>(cx) + 1.0f) * zones.cellSize;
    const f32 sz = (static_cast<f32>(cz) + 1.0f) * zones.cellSize;
    const auto crosses = [&](f32 across, f32 along, bool vertical) {
        const i32 line = static_cast<i32>(std::lround(across / zones.mapSize));
        if (std::abs(across - static_cast<f32>(line) * zones.mapSize) >
            0.5f * zones.cellSize + 1.0f) {
            return false;
        }
        const i32 cell = static_cast<i32>(std::floor(along / zones.mapSize));
        return mapBorderSegmentStyle(world, zones.mapSize, line, cell,
                                     vertical) == MapEdgeStyle::Ridges;
    };
    return crosses(sx, sz, true) || crosses(sz, sx, false);
}

const Zone& zoneCell(const WorldLayerParams& world, const ZoneParams& zones,
                     i32 cx, i32 cz) {
    Memo& memo = memoFor(world, zones);
    const u64 key = cellKey(cx, cz);
    if (const auto it = memo.cells.find(key); it != memo.cells.end()) {
        return it->second;
    }
    const RawCell raw = rawCell(world, zones, cx, cz);
    Zone zone;
    zone.x = raw.x;
    zone.z = raw.z;
    zone.hash = raw.hash;
    zone.pieceScale = glm::mix(0.8f, 1.25f, roll01(zone.hash, 3));
    zone.archetype = raw.archetype;
    zone.storey = raw.storey;
    if (!raw.sea && !raw.startMeadow) {
        const vector<ZoneArchetype>& table = zoneArchetypesOf(zones);
        // The bowl rule: a zone never sits below ALL its neighbours
        // unless its archetype holds water (a basin, a marsh) — every
        // other local minimum would be a lake by construction.
        i32 lowest = 1 << 20;
        i32 highest = -(1 << 20);
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const RawCell n = rawCell(world, zones, cx + dx, cz + dz);
                lowest = glm::min(lowest, n.storey);
                highest = glm::max(highest, n.storey);
            }
        }
        if (!table.empty() && !holdsWater(table[zone.archetype])) {
            zone.storey = glm::max(zone.storey, lowest);
        }
        // The rampart rule: the row a Ridges map line crosses never
        // sits below any neighbour. The border range (TerrainGen's
        // additive lift) then rises out of a divide: no zone wall
        // drops TOWARD the line, so no trough runs along its foot for
        // the master drainage to follow (the river-along-the-border
        // bug). A Sea segment keeps its shore.
        if (zones.mapSize > 0.0f &&
            ridgeLineCrossesCell(world, zones, cx, cz)) {
            zone.storey = glm::clamp(
                glm::max(zone.storey, highest) + zones.rampartSteps, 0,
                static_cast<i32>(zones.storeys));
        }
        const f32 gate = noise::smoothstep01(
            world.startRadius + zones.startGap * 0.25f,
            world.startRadius + zones.startGap, raw.dStart);
        zone.storeyHeight =
            static_cast<f32>(zone.storey) * zones.stepHeight * gate;
    }
    return memo.cells.emplace(key, zone).first->second;
}

struct Nearest {
    const Zone* a { nullptr }; // nearest
    const Zone* b { nullptr }; // second nearest
    f32 da { 1.0e30f };
    f32 db { 1.0e30f };
    // The nine candidates (the 3x3 cells around the warped point).
    const Zone* zones[9] {};
    f32 dist[9] {};
    u32 count { 0 };
};

Nearest nearestZones(const WorldLayerParams& world, const ZoneParams& zones,
                     f32 x, f32 z) {
    // Warped query: the Voronoi borders wander instead of running
    // straight between two sites.
    const f32 wx =
        x + (noise::fbm(world.seed ^ kSaltZoneWarpX, x, z,
                        1.0f / zones.borderWarpWavelength, 2, 2.0f, 0.5f) -
             0.5f) *
                2.0f * zones.borderWarp;
    const f32 wz =
        z + (noise::fbm(world.seed ^ kSaltZoneWarpZ, x, z,
                        1.0f / zones.borderWarpWavelength, 2, 2.0f, 0.5f) -
             0.5f) *
                2.0f * zones.borderWarp;
    const i32 cx = static_cast<i32>(std::floor(wx / zones.cellSize - 0.5f));
    const i32 cz = static_cast<i32>(std::floor(wz / zones.cellSize - 0.5f));
    Nearest n;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const Zone& zone = zoneCell(world, zones, cx + dx, cz + dz);
            const f32 d = std::hypot(wx - zone.x, wz - zone.z);
            n.zones[n.count] = &zone;
            n.dist[n.count] = d;
            ++n.count;
            if (d < n.da) {
                n.b = n.a;
                n.db = n.da;
                n.a = &zone;
                n.da = d;
            } else if (d < n.db) {
                n.b = &zone;
                n.db = d;
            }
        }
    }
    return n;
}

// The zone's small landform at (x, z) from its site: a knoll, a
// clearing, a pond, a rock knob, a grove mound (the August intimate
// grid, one per zone, typed by the archetype).
void pieceAt(const ZoneParams& zones, const ZoneArchetype& a,
             const Zone& zone, f32 x, f32 z, ZoneSample& out) {
    if (a.piece == 0) {
        return;
    }
    // The table's radii are written for 1 km zones: a piece keeps its
    // share of the zone whatever the cell size.
    const f32 radius = glm::max(a.pieceRadius * zone.pieceScale *
                                    (zones.cellSize / 1000.0f),
                                10.0f);
    const f32 height = a.pieceHeight * zone.pieceScale;
    const f32 n = std::hypot(x - zone.x, z - zone.z) / radius;
    if (n >= 1.0f) {
        return;
    }
    const f32 k = 1.0f - n;
    switch (a.piece) {
    case 1: // knoll: a smooth dome
    case 5: // grove mound: a low dome
        out.pieceLift = glm::max(out.pieceLift, height * k * k * (3.0f - 2.0f * k));
        break;
    case 2: // clearing: a relief-suppression bowl, no lift
        out.pieceClearing = glm::max(out.pieceClearing,
                                     1.0f - noise::smoothstep01(0.6f, 1.0f, n));
        break;
    case 3: // pond: a flat-floored basin
        out.pieceBasin = glm::max(out.pieceBasin,
                                  height * (1.0f - noise::smoothstep01(0.4f, 1.0f, n)));
        break;
    case 4: // rock knob: a steep little cone
        out.pieceLift = glm::max(out.pieceLift, height * k);
        break;
    default:
        break;
    }
}

} // namespace

const vector<ZoneArchetype>& defaultZoneArchetypes() {
    // docs/POI-CATALOGUE.md §E, temperate-first (the climate palettes
    // stay the world layer's); the conditions read WorldSample::etage,
    // massif, coast, moisture at the zone's site.
    static const vector<ZoneArchetype> table = [] {
        vector<ZoneArchetype> t;
        const auto row = [&](const char* name, f32 weight, f32 e0, f32 e1,
                             f32 m0, f32 m1, f32 c0, f32 c1, f32 h0, f32 h1,
                             i32 bias, f32 relief, f32 wavelength,
                             f32 terrace, f32 step, f32 crests, f32 hard,
                             f32 wet, f32 cover, u32 palette, u32 piece,
                             f32 pieceH, f32 pieceR, f32 cut) {
            ZoneArchetype a;
            a.name = name;
            a.weight = weight;
            a.minEtage = e0;
            a.maxEtage = e1;
            a.minMassif = m0;
            a.maxMassif = m1;
            a.minCoast = c0;
            a.maxCoast = c1;
            a.minMoisture = h0;
            a.maxMoisture = h1;
            a.storeyBias = bias;
            a.reliefMul = relief;
            a.wavelengthMul = wavelength;
            a.terrace = terrace;
            a.cliffStep = step;
            a.hillCrests = crests;
            a.hardBias = hard;
            a.wetBias = wet;
            a.coverBias = cover;
            a.palette = palette;
            a.piece = piece;
            a.pieceHeight = pieceH;
            a.pieceRadius = pieceR;
            a.erosionCut = cut;
            t.push_back(a);
        };
        //   name           w    etage      massif     coast      moisture   bias relief wl   terr step crest hard  wet   cover pal piece h    r
        row("meadow",       1.2f, 0.0f, 0.62f, 0.0f, 0.5f, 0.0f, 0.5f, 0.3f, 0.8f, 0, 1.0f, 1.0f, 0.15f, 30.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 1, 35.0f, 140.0f, 0.0f);
        row("bocage",       1.0f, 0.0f, 0.55f, 0.0f, 0.4f, 0.0f, 0.5f, 0.45f, 0.9f, 0, 0.8f, 0.8f, 0.1f, 30.0f, 0.0f, 0.0f, 0.1f, 0.0f, 7, 5, 8.0f, 90.0f, 0.0f);
        row("hills",        1.0f, 0.3f, 0.75f, 0.0f, 0.6f, 0.0f, 0.5f, 0.0f, 1.0f, 0, 1.3f, 0.9f, 0.3f, 30.0f, 90.0f, 0.0f, 0.0f, 0.0f, 0, 1, 50.0f, 160.0f, 40.0f);
        row("woodedHills",  0.9f, 0.25f, 0.8f, 0.0f, 0.6f, 0.0f, 0.5f, 0.5f, 1.0f, 0, 1.2f, 0.85f, 0.4f, 30.0f, 0.0f, 0.0f, 0.0f, 0.0f, 6, 2, 0.0f, 150.0f, 20.0f);
        row("mesaPlateau",  0.9f, 0.35f, 0.9f, 0.0f, 0.6f, 0.0f, 0.5f, 0.0f, 0.7f, 1, 0.6f, 1.4f, 0.9f, 40.0f, 0.0f, 0.35f, -0.2f, -0.1f, 5, 4, 15.0f, 45.0f, 15.0f);
        row("highPlateau",  0.6f, 0.5f, 1.0f, 0.0f, 0.7f, 0.0f, 0.5f, 0.0f, 1.0f, 2, 0.7f, 1.2f, 0.6f, 45.0f, 0.0f, 0.2f, -0.1f, 0.2f, 4, 3, 6.0f, 80.0f, 20.0f);
        row("basin",        0.6f, 0.0f, 0.7f, 0.0f, 0.5f, 0.0f, 0.5f, 0.5f, 1.0f, -1, 0.5f, 1.1f, 0.0f, 30.0f, 0.0f, 0.0f, 0.4f, 0.0f, 0, 3, 8.0f, 180.0f, 0.0f);
        row("marsh",        0.5f, 0.0f, 0.45f, 0.0f, 0.3f, 0.0f, 0.5f, 0.6f, 1.0f, 0, 0.35f, 1.2f, 0.0f, 30.0f, 0.0f, -0.1f, 0.6f, 0.2f, 9, 3, 4.0f, 120.0f, 0.0f);
        row("badlands",     0.5f, 0.3f, 0.9f, 0.0f, 0.6f, 0.0f, 0.5f, 0.0f, 0.45f, 0, 1.5f, 0.35f, 0.7f, 15.0f, 0.0f, 0.3f, -0.2f, -0.3f, 5, 4, 20.0f, 50.0f, 120.0f);
        row("rockField",    0.4f, 0.4f, 1.0f, 0.3f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0, 1.0f, 0.9f, 0.5f, 30.0f, 0.0f, 0.4f, -0.1f, 0.1f, 0, 4, 12.0f, 40.0f, 80.0f);
        row("heath",        0.8f, 0.2f, 0.8f, 0.0f, 0.5f, 0.0f, 0.5f, 0.3f, 0.7f, 0, 0.9f, 1.8f, 0.5f, 30.0f, 0.0f, 0.1f, -0.1f, 0.3f, 4, 1, 25.0f, 150.0f, 10.0f);
        row("ridgeCountry", 1.0f, 0.3f, 1.0f, 0.5f, 1.0f, 0.0f, 0.5f, 0.0f, 1.0f, 1, 1.4f, 1.0f, 0.5f, 35.0f, 150.0f, 0.3f, -0.1f, 0.0f, 0, 4, 25.0f, 60.0f, 160.0f);
        row("coastCliffs",  1.0f, 0.25f, 1.0f, 0.0f, 1.0f, 0.5f, 1.0f, 0.0f, 1.0f, 1, 0.8f, 1.0f, 0.8f, 40.0f, 0.0f, 0.3f, 0.0f, 0.0f, 0, 4, 15.0f, 50.0f, 60.0f);
        row("lowCoast",     1.0f, 0.0f, 0.4f, 0.0f, 0.4f, 0.5f, 1.0f, 0.0f, 1.0f, 0, 0.4f, 0.6f, 0.0f, 30.0f, 0.0f, -0.1f, 0.0f, -0.2f, 5, 1, 12.0f, 80.0f, 0.0f);
        return t;
    }();
    return table;
}

const vector<ZoneArchetype>& zoneArchetypesOf(const ZoneParams& zones) {
    return zones.archetypes.empty() ? defaultZoneArchetypes()
                                    : zones.archetypes;
}

ZoneSample zoneSampleAt(const WorldLayerParams& world, const ZoneParams& zones,
                        f32 x, f32 z) {
    // The synthesis asks twice per texel (the floor, then the palette):
    // the last answer is kept.
    struct LastSample {
        u64 key { 0 };
        f32 x { 0.0f }, z { 0.0f };
        bool valid { false };
        ZoneSample sample;
    };
    thread_local LastSample last;
    const u64 key = paramsKey(world, zones);
    if (last.valid && last.key == key && last.x == x && last.z == z) {
        return last.sample;
    }
    ZoneSample out;
    const Nearest n = nearestZones(world, zones, x, z);
    if (!n.a) {
        return out;
    }
    const vector<ZoneArchetype>& table = zoneArchetypesOf(zones);
    const Zone& a = *n.a;
    const Zone* b = n.b ? n.b : n.a;
    const ZoneArchetype& ga = table[glm::min<size_t>(a.archetype, table.size() - 1)];
    const ZoneArchetype& gb = table[glm::min<size_t>(b->archetype, table.size() - 1)];
    out.archetype = a.archetype;
    out.storey = a.storey;
    out.borderDist = 0.5f * (n.db - n.da);
    // The wall: the storeys meet across a riser whose width follows
    // the step count (one step = a band to scramble, more = an
    // escarpment); the corridors' ramp is wider and linear-ish.
    out.wallSteps = static_cast<f32>(std::abs(a.storey - b->storey));
    // The floor: a smooth PARTITION over the nine sites — a site's
    // weight fades out as it falls behind the nearest by the riser
    // width — continuous everywhere (bisectors and corners alike; the
    // former "nearest two" formulation kinked at every Voronoi vertex
    // by half a storey). The riser width per site follows the step
    // count against the nearest zone.
    const auto widthFor = [&](const Zone& other) {
        const f32 steps = static_cast<f32>(std::abs(a.storey - other.storey));
        return steps >= 2.0f ? zones.wallWidthHigh * steps : zones.wallWidthOne;
    };
    f32 wsum = 0.0f, hsum = 0.0f, ssum = 0.0f, wsumS = 0.0f;
    for (u32 i = 0; i < n.count; ++i) {
        const Zone& zi = *n.zones[i];
        const f32 behind = n.dist[i] - n.da;
        // ONE blend width for every site: a width that depended on the
        // nearest zone jumped when the nearest changed (a 10 m kink on
        // the bisectors). The per-step width only sizes the wall MASK.
        const f32 w = 1.0f - noise::smoothstep01(0.0f, zones.wallWidthOne, behind);
        wsum += w;
        hsum += w * zi.storeyHeight;
        const f32 ws = 1.0f - noise::smoothstep01(0.0f, zones.rampWidth, behind);
        wsumS += ws;
        ssum += ws * zi.storeyHeight;
    }
    out.storeyHeight = wsum > 0.0f ? hsum / wsum : a.storeyHeight;
    out.storeyHeightSmooth = wsumS > 0.0f ? ssum / wsumS : a.storeyHeight;
    const f32 width = widthFor(*b);
    out.wall = out.wallSteps > 0.0f && a.storeyHeight != b->storeyHeight
                   ? 1.0f - noise::smoothstep01(0.35f * width, 0.6f * width,
                                                std::abs(out.borderDist))
                   : 0.0f;
    // The grammar: the nearest zone's, blended over ~150 m at a border.
    const f32 tg = noise::smoothstep01(-75.0f, 75.0f, out.borderDist);
    out.reliefMul = glm::mix(gb.reliefMul, ga.reliefMul, tg);
    out.wavelengthMul = glm::mix(gb.wavelengthMul, ga.wavelengthMul, tg);
    out.terrace = glm::mix(gb.terrace, ga.terrace, tg);
    out.cliffStep = glm::mix(gb.cliffStep, ga.cliffStep, tg);
    out.hillCrests = glm::mix(gb.hillCrests, ga.hillCrests, tg);
    out.hardBias = glm::mix(gb.hardBias, ga.hardBias, tg);
    out.wetBias = glm::mix(gb.wetBias, ga.wetBias, tg);
    out.coverBias = glm::mix(gb.coverBias, ga.coverBias, tg);
    out.erosionCut = glm::mix(gb.erosionCut, ga.erosionCut, tg);
    out.palette = ga.palette;
    pieceAt(zones, ga, a, x, z, out);
    if (b != n.a) {
        pieceAt(zones, gb, *b, x, z, out);
    }
    last.key = key;
    last.x = x;
    last.z = z;
    last.valid = true;
    last.sample = out;
    return out;
}

u32 zoneArchetypeAt(const WorldLayerParams& world, const ZoneParams& zones,
                    f32 x, f32 z) {
    const Nearest n = nearestZones(world, zones, x, z);
    return n.a ? n.a->archetype : 0u;
}

u64 hashParams(const ZoneParams& p) {
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
    const auto i = [&](i32 v) { mix(&v, sizeof(v)); };
    f(p.cellSize);
    f(p.jitter);
    f(p.borderWarp);
    f(p.borderWarpWavelength);
    f(p.stepHeight);
    u(p.storeys);
    f(p.trendWavelength);
    f(p.trendContrast);
    f(p.wallWidthOne);
    f(p.wallWidthHigh);
    f(p.rampWidth);
    f(p.startGap);
    f(p.mapSize);
    i(p.rampartSteps);
    for (const ZoneArchetype& a : zoneArchetypesOf(p)) {
        mix(a.name.data(), a.name.size());
        f(a.weight);
        f(a.minEtage);
        f(a.maxEtage);
        f(a.minMassif);
        f(a.maxMassif);
        f(a.minCoast);
        f(a.maxCoast);
        f(a.minMoisture);
        f(a.maxMoisture);
        i(a.storeyBias);
        f(a.reliefMul);
        f(a.wavelengthMul);
        f(a.terrace);
        f(a.cliffStep);
        f(a.hillCrests);
        f(a.hardBias);
        f(a.wetBias);
        f(a.coverBias);
        u(a.palette);
        u(a.piece);
        f(a.pieceHeight);
        f(a.pieceRadius);
        f(a.erosionCut);
    }
    return h;
}

} // namespace render::terraingen
