#include "engine/terrain/generation/PoiPlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/Hash.hpp"
#include "engine/terrain/Noise.hpp"

namespace render::terraingen {

const char* poiTypeName(PoiType type) {
    static const char* kNames[] = {
        "summit",    "needle",   "ridge",     "mesa",      "butte",
        "escarpment", "canyon",  "col",       "hoodoos",   "karst-towers",
        "boulders",  "crater",   "cirque",    "waterfall", "plain-lake",
        "tarn",      "spring",   "oasis",     "confluence", "headland",
        "sea-cliff", "sea-stack", "cove",     "fjord",     "islet",
        "lone-tree", "grove",    "city-pad",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                  static_cast<size_t>(PoiType::Count));
    return kNames[static_cast<size_t>(type)];
}

namespace {

constexpr u32 kSaltTier[3] = { 0x9a0d0001u, 0x9a0d0002u, 0x9a0d0003u };

struct Weighted {
    PoiType type;
    f32 weight;
};

// Weighted draw by a [0,1) roll.
PoiType pick(const Weighted* table, size_t n, f32 roll) {
    f32 total = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        total += table[i].weight;
    }
    f32 acc = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        acc += table[i].weight;
        if (roll * total < acc) {
            return table[i].type;
        }
    }
    return table[n - 1].type;
}

f32 roll01(u32 h, u32 k) {
    return static_cast<f32>(core::hashU32(h + k * 0x9e3779b9u)) *
           (1.0f / 4294967295.0f);
}

// The catalogue's tables (docs/POI-CATALOGUE.md §F1): the type of a
// site from its tier and its world sample. `alt` re-rolls (rules F2/F3).
PoiType drawType(PoiTier tier, const WorldSample& w, bool startDisc,
                 u32 hash, u32 alt) {
    const f32 r = roll01(hash, 11 + alt);
    const f32 base = glm::max(w.base, 0.0f);
    const bool arid = w.moisture < 0.38f && w.temperature > 0.58f;
    const bool cold = w.temperature < 0.34f;
    if (tier == PoiTier::Grand) {
        if (w.sea) {
            return w.coast > 0.4f && w.massif > 0.3f ? PoiType::Fjord
                                                      : PoiType::Islet;
        }
        if (startDisc) {
            return PoiType::Summit;
        }
        if (w.coast > 0.5f) {
            return PoiType::Headland;
        }
        if (w.massif > 0.4f) {
            const Weighted t[] = { { PoiType::Summit, 70.0f },
                                   { PoiType::Cirque, 15.0f },
                                   { PoiType::Needle, 15.0f } };
            return pick(t, 3, r);
        }
        if (base >= 450.0f) {
            const Weighted t[] = { { PoiType::Mesa, 55.0f },
                                   { PoiType::Summit, 30.0f },
                                   { PoiType::Canyon, 15.0f } };
            return pick(t, 3, r);
        }
        const Weighted t[] = { { PoiType::Summit, 55.0f },
                               { PoiType::Mesa, 25.0f },
                               { PoiType::Crater, arid ? 20.0f : 8.0f } };
        return pick(t, 3, r);
    }
    if (tier == PoiTier::Petit) {
        const Weighted t[] = { { PoiType::Boulders, cold ? 45.0f : 25.0f },
                               { PoiType::Spring, 20.0f },
                               { PoiType::LoneTree, arid ? 5.0f : 20.0f },
                               { PoiType::Grove, arid ? 5.0f : 15.0f },
                               { PoiType::Butte, 20.0f } };
        return pick(t, 5, r);
    }
    // Moyen.
    if (w.sea) {
        return PoiType::Islet;
    }
    if (startDisc) {
        const Weighted t[] = { { PoiType::Butte, 30.0f },
                               { PoiType::Ridge, 15.0f },
                               { PoiType::Grove, 15.0f },
                               { PoiType::LoneTree, 10.0f },
                               { PoiType::PlainLake, 15.0f },
                               { PoiType::CityPad, 15.0f } };
        return pick(t, 6, r);
    }
    if (w.coast > 0.5f) {
        const Weighted t[] = { { PoiType::Headland, 35.0f },
                               { PoiType::SeaCliff, w.massif > 0.2f ||
                                                            base > 60.0f
                                                        ? 25.0f
                                                        : 10.0f },
                               { PoiType::Cove, 25.0f },
                               { PoiType::LoneTree, 10.0f } };
        return pick(t, 4, r);
    }
    if (w.massif > 0.5f) {
        const Weighted t[] = { { PoiType::Summit, 35.0f },
                               { PoiType::Needle, 20.0f },
                               { PoiType::Ridge, 20.0f },
                               { PoiType::Col, 10.0f },
                               { PoiType::Cirque, base > 650.0f ? 15.0f
                                                                : 0.0f },
                               { PoiType::Tarn, base > 650.0f ? 10.0f
                                                              : 0.0f } };
        return pick(t, 6, r);
    }
    if (base >= 450.0f) {
        const Weighted t[] = { { PoiType::Mesa, 35.0f },
                               { PoiType::Escarpment, 20.0f },
                               { PoiType::Canyon, 20.0f },
                               { PoiType::KarstTowers, arid ? 0.0f : 10.0f },
                               { PoiType::Hoodoos, arid ? 15.0f : 0.0f },
                               { PoiType::Butte, 15.0f } };
        return pick(t, 6, r);
    }
    if (base >= 150.0f) {
        const Weighted t[] = { { PoiType::Butte, 25.0f },
                               { PoiType::Ridge, 25.0f },
                               { PoiType::Mesa, 15.0f },
                               { PoiType::Waterfall, 15.0f },
                               { PoiType::PlainLake, 10.0f },
                               { PoiType::Grove, arid ? 0.0f : 10.0f },
                               { PoiType::Oasis, arid ? 10.0f : 0.0f } };
        return pick(t, 7, r);
    }
    const Weighted t[] = { { PoiType::Butte, 30.0f },
                           { PoiType::PlainLake, 20.0f },
                           { PoiType::Ridge, 10.0f },
                           { PoiType::Mesa, 10.0f },
                           { PoiType::Grove, arid ? 0.0f : 10.0f },
                           { PoiType::LoneTree, 10.0f },
                           { PoiType::CityPad, 10.0f },
                           { PoiType::Oasis, arid ? 10.0f : 0.0f } };
    return pick(t, 8, r);
}

bool basinType(PoiType type) {
    return type == PoiType::PlainLake || type == PoiType::Tarn ||
           type == PoiType::Cove || type == PoiType::Cirque;
}

struct CellKey {
    PoiTier tier;
    i32 cx;
    i32 cz;
};

u64 keyOf(const CellKey& k) {
    return (static_cast<u64>(static_cast<u32>(k.cx)) << 32) ^
           static_cast<u64>(static_cast<u32>(k.cz)) ^
           (static_cast<u64>(k.tier) << 61);
}

struct CellSite {
    bool present { false };
    PoiSite site;
};

f32 cellSize(const PoiPlanParams& plan, PoiTier tier) {
    switch (tier) {
    case PoiTier::Grand: return plan.grandCell;
    case PoiTier::Moyen: return plan.moyenCell;
    default: return plan.petitCell;
    }
}

// Every lattice is map-aligned (the grand cell IS the 8 km map); the
// jitter margins keep a site off the lines (a grand sits >= 2.4 km in).
f32 cellOffset(const PoiPlanParams&, PoiTier) {
    return 0.0f;
}

// Position + hash of a cell's site (type-free: the edges read it).
CellSite placeSite(const WorldLayerParams& world, const PoiPlanParams& plan,
                   PoiTier tier, i32 cx, i32 cz) {
    CellSite out;
    const u32 salt = world.seed ^ kSaltTier[static_cast<u32>(tier)];
    const auto jitter = [&](u32 k) {
        return noise::lattice(salt + k * 0x9e3779b9u, cx, cz);
    };
    if (tier == PoiTier::Petit && jitter(9) > plan.petitChance) {
        return out;
    }
    const f32 size = cellSize(plan, tier);
    const f32 offset = cellOffset(plan, tier);
    const f32 lo = tier == PoiTier::Petit ? 0.2f
                   : tier == PoiTier::Moyen ? 0.25f
                                            : 0.3f;
    const f32 hi = 1.0f - lo;
    PoiSite& s = out.site;
    s.tier = tier;
    s.cellX = cx;
    s.cellZ = cz;
    s.x = (static_cast<f32>(cx) + glm::mix(lo, hi, jitter(0))) * size -
          offset;
    s.z = (static_cast<f32>(cz) + glm::mix(lo, hi, jitter(1))) * size -
          offset;
    s.hash = core::hashU32(salt ^ core::hashU32(static_cast<u32>(cx) ^
                                                 (static_cast<u32>(cz) *
                                                  0x85ebca6bu)));
    // The start cell's grand stands clear of the spawn (the probe lands
    // on the meadow, never on its flank): pushed away along the ray.
    if (tier == PoiTier::Grand) {
        const f32 dx = s.x - world.startX;
        const f32 dz = s.z - world.startZ;
        const f32 d = std::hypot(dx, dz);
        if (d < plan.grandStartClearance) {
            const f32 ang = d > 1.0f ? std::atan2(dz, dx)
                                     : jitter(2) * 6.2831853f;
            s.x = world.startX + std::cos(ang) * plan.grandStartClearance;
            s.z = world.startZ + std::sin(ang) * plan.grandStartClearance;
        }
    }
    out.present = true;
    return out;
}

f32 lerpHash(f32 lo, f32 hi, u32 hash, u32 k) {
    return glm::mix(lo, hi, roll01(hash, k));
}

// Size and shape of a typed site.
void sizeSite(const PoiPlanParams& plan, PoiSite& s) {
    switch (s.tier) {
    case PoiTier::Grand:
        s.radius = lerpHash(1200.0f, 2000.0f, s.hash, 3);
        s.height = lerpHash(plan.grandHeightMin, plan.grandHeightMax,
                            s.hash, 4);
        break;
    case PoiTier::Moyen:
        s.radius = lerpHash(350.0f, 900.0f, s.hash, 3);
        s.height = lerpHash(plan.moyenHeightMin, plan.moyenHeightMax,
                            s.hash, 4);
        break;
    default:
        s.radius = lerpHash(30.0f, 80.0f, s.hash, 3);
        s.height = lerpHash(plan.petitHeightMin, plan.petitHeightMax,
                            s.hash, 4);
        break;
    }
    if (basinType(s.type)) {
        s.height = -lerpHash(5.0f, 15.0f, s.hash, 5); // depth
        if (s.type == PoiType::Tarn) {
            s.radius = lerpHash(100.0f, 300.0f, s.hash, 3);
        }
    } else if (s.type == PoiType::CityPad || s.type == PoiType::Confluence) {
        s.height = 0.0f;
        s.radius = lerpHash(150.0f, 300.0f, s.hash, 3);
    } else if (s.type == PoiType::LoneTree || s.type == PoiType::Spring) {
        s.height = glm::min(s.height, 12.0f);
        s.radius = glm::min(s.radius, 60.0f);
    } else if (s.type == PoiType::Islet || s.type == PoiType::SeaStack) {
        s.height = glm::min(s.height, 80.0f);
        s.radius = glm::min(s.radius, 300.0f);
    }
}

bool startDiscAt(const WorldLayerParams& world, f32 x, f32 z) {
    return std::hypot(x - world.startX, z - world.startZ) <
           world.startLowRadius + world.startLowFade;
}

// Lexicographic rank of a cell: the lower-ranked neighbour keeps its
// draw, the higher re-rolls (rules F2/F3, one level deep).
bool lowerRank(i32 ax, i32 az, i32 bx, i32 bz) {
    return az < bz || (az == bz && ax < bx);
}

// Memo of typed sites (and the moyen edges) per cell. Thread-local, so
// workers never share it; re-keyed by the params that shape it.
struct Memo {
    u32 paramsKey { 0 };
    std::unordered_map<u64, CellSite> sites;
    std::unordered_map<u64, vector<PoiSite>> rawEdges; // moyen: partners
};

u32 paramsKeyOf(const WorldLayerParams& world, const PoiPlanParams& plan) {
    u32 h = core::hashU32(world.seed);
    const auto mixF = [&](f32 v) {
        u32 bits;
        static_assert(sizeof(bits) == sizeof(v));
        std::memcpy(&bits, &v, sizeof(bits));
        h = core::hashU32(h ^ bits);
    };
    mixF(world.startX);
    mixF(world.startZ);
    mixF(world.startLowRadius);
    mixF(world.startLowFade);
    mixF(plan.grandCell);
    mixF(plan.moyenCell);
    mixF(plan.petitCell);
    mixF(plan.petitChance);
    mixF(plan.grandHeightMin);
    mixF(plan.grandHeightMax);
    mixF(plan.moyenHeightMin);
    mixF(plan.moyenHeightMax);
    mixF(plan.petitHeightMin);
    mixF(plan.petitHeightMax);
    mixF(plan.edgeReach);
    mixF(static_cast<f32>(plan.edgeMax));
    mixF(plan.grandStartClearance);
    return h;
}

Memo& memoFor(const WorldLayerParams& world, const PoiPlanParams& plan) {
    thread_local Memo memo;
    const u32 key = paramsKeyOf(world, plan);
    if (memo.paramsKey != key || memo.sites.size() > 8192) {
        memo.sites.clear();
        memo.rawEdges.clear();
        memo.paramsKey = key;
    }
    return memo;
}

// The alt-0 draw of a cell (no neighbour rule): what the rules compare.
PoiType baseDraw(const WorldLayerParams& world, PoiTier tier,
                 const PoiSite& s) {
    const WorldSample w = worldSampleAt(world, s.x, s.z);
    return drawType(tier, w, startDiscAt(world, s.x, s.z), s.hash, 0);
}

const CellSite& typedSite(const WorldLayerParams& world,
                          const PoiPlanParams& plan, PoiTier tier, i32 cx,
                          i32 cz);

// Partners of a moyen cell's site: its k nearest within reach that pass
// the relative-neighbourhood test (no third site closer to both).
const vector<PoiSite>& rawEdgesOf(const WorldLayerParams& world,
                                  const PoiPlanParams& plan, i32 cx,
                                  i32 cz) {
    Memo& memo = memoFor(world, plan);
    const u64 key = keyOf({ PoiTier::Moyen, cx, cz });
    if (const auto it = memo.rawEdges.find(key); it != memo.rawEdges.end()) {
        return it->second;
    }
    vector<PoiSite> partners;
    const CellSite& self = typedSite(world, plan, PoiTier::Moyen, cx, cz);
    if (self.present) {
        const i32 reachCells = static_cast<i32>(
            std::ceil(plan.edgeReach / plan.moyenCell));
        vector<PoiSite> candidates;
        for (i32 dz = -reachCells; dz <= reachCells; ++dz) {
            for (i32 dx = -reachCells; dx <= reachCells; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const CellSite& c =
                    typedSite(world, plan, PoiTier::Moyen, cx + dx, cz + dz);
                if (!c.present) {
                    continue;
                }
                if (std::hypot(c.site.x - self.site.x,
                               c.site.z - self.site.z) <= plan.edgeReach) {
                    candidates.push_back(c.site);
                }
            }
        }
        const auto dist = [&](const PoiSite& a, const PoiSite& b) {
            return std::hypot(a.x - b.x, a.z - b.z);
        };
        std::sort(candidates.begin(), candidates.end(),
                  [&](const PoiSite& a, const PoiSite& b) {
                      const f32 da = dist(a, self.site);
                      const f32 db = dist(b, self.site);
                      return da < db ||
                             (da == db && lowerRank(a.cellX, a.cellZ,
                                                    b.cellX, b.cellZ));
                  });
        for (const PoiSite& t : candidates) {
            if (partners.size() >= plan.edgeMax) {
                break;
            }
            const f32 d = dist(t, self.site);
            bool blocked = false;
            for (const PoiSite& c : candidates) {
                if (c.cellX == t.cellX && c.cellZ == t.cellZ) {
                    continue;
                }
                if (dist(c, self.site) < d && dist(c, t) < d) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                partners.push_back(t);
            }
        }
    }
    return memo.rawEdges.emplace(key, std::move(partners)).first->second;
}

const CellSite& typedSite(const WorldLayerParams& world,
                          const PoiPlanParams& plan, PoiTier tier, i32 cx,
                          i32 cz) {
    Memo& memo = memoFor(world, plan);
    const u64 key = keyOf({ tier, cx, cz });
    if (const auto it = memo.sites.find(key); it != memo.sites.end()) {
        return it->second;
    }
    CellSite cell = placeSite(world, plan, tier, cx, cz);
    if (cell.present) {
        PoiSite& s = cell.site;
        const WorldSample w = worldSampleAt(world, s.x, s.z);
        const bool startDisc = startDiscAt(world, s.x, s.z);
        u32 alt = 0;
        s.type = drawType(tier, w, startDisc, s.hash, alt);
        if (tier == PoiTier::Grand) {
            // F2: never the same grand as a lower-ranked neighbour.
            for (u32 tries = 0; tries < 3; ++tries) {
                bool clash = false;
                for (i32 dz = -1; dz <= 1 && !clash; ++dz) {
                    for (i32 dx = -1; dx <= 1; ++dx) {
                        if ((dx == 0 && dz == 0) ||
                            !lowerRank(cx + dx, cz + dz, cx, cz)) {
                            continue;
                        }
                        const CellSite n = placeSite(world, plan, tier,
                                                     cx + dx, cz + dz);
                        if (n.present &&
                            baseDraw(world, tier, n.site) == s.type) {
                            clash = true;
                            break;
                        }
                    }
                }
                if (!clash) {
                    break;
                }
                s.type = drawType(tier, w, startDisc, s.hash, ++alt);
            }
        } else if (tier == PoiTier::Moyen) {
            // F3: never the same type as a lower-ranked graph neighbour
            // (compared on their base draw: one level, no recursion).
            // The partner set is position-only, so it is safe to read
            // here through placeSite (no typedSite re-entry).
            const i32 reachCells = static_cast<i32>(
                std::ceil(plan.edgeReach / plan.moyenCell));
            for (u32 tries = 0; tries < 3; ++tries) {
                bool clash = false;
                for (i32 dz = -reachCells; dz <= reachCells && !clash; ++dz) {
                    for (i32 dx = -reachCells; dx <= reachCells; ++dx) {
                        if ((dx == 0 && dz == 0) ||
                            !lowerRank(cx + dx, cz + dz, cx, cz)) {
                            continue;
                        }
                        const CellSite n = placeSite(world, plan, tier,
                                                     cx + dx, cz + dz);
                        if (!n.present ||
                            std::hypot(n.site.x - s.x, n.site.z - s.z) >
                                plan.edgeReach) {
                            continue;
                        }
                        if (baseDraw(world, tier, n.site) == s.type) {
                            clash = true;
                            break;
                        }
                    }
                }
                if (!clash) {
                    break;
                }
                s.type = drawType(tier, w, startDisc, s.hash, ++alt);
            }
        }
        sizeSite(plan, s);
    }
    return memo.sites.emplace(key, cell).first->second;
}

} // namespace

vector<PoiSite> poiSitesNear(const WorldLayerParams& world,
                             const PoiPlanParams& plan, f32 minX, f32 minZ,
                             f32 maxX, f32 maxZ) {
    vector<PoiSite> out;
    for (const PoiTier tier :
         { PoiTier::Grand, PoiTier::Moyen, PoiTier::Petit }) {
        const f32 size = cellSize(plan, tier);
        const f32 offset = cellOffset(plan, tier);
        // The grand may be pushed by the start clearance: one cell of
        // slack on the scan.
        const i32 slack = tier == PoiTier::Grand ? 1 : 0;
        const i32 cx0 = static_cast<i32>(std::floor((minX + offset) / size)) - slack;
        const i32 cx1 = static_cast<i32>(std::floor((maxX + offset) / size)) + slack;
        const i32 cz0 = static_cast<i32>(std::floor((minZ + offset) / size)) - slack;
        const i32 cz1 = static_cast<i32>(std::floor((maxZ + offset) / size)) + slack;
        for (i32 cz = cz0; cz <= cz1; ++cz) {
            for (i32 cx = cx0; cx <= cx1; ++cx) {
                const CellSite& c = typedSite(world, plan, tier, cx, cz);
                if (c.present && c.site.x >= minX && c.site.x < maxX &&
                    c.site.z >= minZ && c.site.z < maxZ) {
                    out.push_back(c.site);
                }
            }
        }
    }
    return out;
}

vector<PoiEdge> poiEdgesNear(const WorldLayerParams& world,
                             const PoiPlanParams& plan, f32 minX, f32 minZ,
                             f32 maxX, f32 maxZ) {
    vector<PoiEdge> out;
    const f32 size = plan.moyenCell;
    const i32 reachCells =
        static_cast<i32>(std::ceil(plan.edgeReach / size));
    const i32 cx0 = static_cast<i32>(std::floor(minX / size)) - reachCells;
    const i32 cx1 = static_cast<i32>(std::floor(maxX / size)) + reachCells;
    const i32 cz0 = static_cast<i32>(std::floor(minZ / size)) - reachCells;
    const i32 cz1 = static_cast<i32>(std::floor(maxZ / size)) + reachCells;
    const auto inRect = [&](const PoiSite& s) {
        return s.x >= minX && s.x < maxX && s.z >= minZ && s.z < maxZ;
    };
    for (i32 cz = cz0; cz <= cz1; ++cz) {
        for (i32 cx = cx0; cx <= cx1; ++cx) {
            const CellSite& self = typedSite(world, plan, PoiTier::Moyen, cx, cz);
            if (!self.present) {
                continue;
            }
            for (const PoiSite& t : rawEdgesOf(world, plan, cx, cz)) {
                // Canonical endpoint emits; symmetric: the partner must
                // list us too (its k-nearest may differ).
                if (!lowerRank(cx, cz, t.cellX, t.cellZ)) {
                    continue;
                }
                bool mutual = false;
                for (const PoiSite& back :
                     rawEdgesOf(world, plan, t.cellX, t.cellZ)) {
                    if (back.cellX == cx && back.cellZ == cz) {
                        mutual = true;
                        break;
                    }
                }
                if (!mutual || (!inRect(self.site) && !inRect(t))) {
                    continue;
                }
                PoiEdge e;
                e.a = self.site;
                e.b = t;
                const f32 mx = (e.a.x + e.b.x) * 0.5f;
                const f32 mz = (e.a.z + e.b.z) * 0.5f;
                const f32 dx = e.b.x - e.a.x;
                const f32 dz = e.b.z - e.a.z;
                const f32 len = std::hypot(dx, dz);
                const u32 eh = core::hashU32(e.a.hash ^ (e.b.hash * 0x27d4eb2fu));
                const f32 bend = glm::mix(-0.25f, 0.25f, roll01(eh, 1));
                e.wx = mx + (-dz / glm::max(len, 1.0f)) * bend * len;
                e.wz = mz + (dx / glm::max(len, 1.0f)) * bend * len;
                out.push_back(e);
            }
        }
    }
    return out;
}

} // namespace render::terraingen
