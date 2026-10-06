#include "engine/terrain/generation/PoiPlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/glm.hpp>

#include "engine/core/Hash.hpp"
#include "engine/terrain/Noise.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

namespace render::terraingen {

const char* poiCharacterName(PoiCharacter c) {
    static const char* kNames[] = { "rolling-meadow", "bocage",
                                    "wooded-hills",   "marsh",
                                    "rocky-plateau",  "heath" };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) ==
                  static_cast<size_t>(PoiCharacter::Count));
    return kNames[static_cast<size_t>(c)];
}

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
        // Low country around the start: no massif types, but its
        // water (a step, a confluence) and its lakes.
        const Weighted t[] = { { PoiType::Butte, 26.0f },
                               { PoiType::Ridge, 14.0f },
                               { PoiType::Grove, 12.0f },
                               { PoiType::LoneTree, 8.0f },
                               { PoiType::PlainLake, 14.0f },
                               { PoiType::Waterfall, 10.0f },
                               { PoiType::Confluence, 6.0f },
                               { PoiType::CityPad, 10.0f } };
        return pick(t, 8, r);
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
        const Weighted t[] = { { PoiType::Summit, 30.0f },
                               { PoiType::Needle, 18.0f },
                               { PoiType::Ridge, 18.0f },
                               { PoiType::Col, 10.0f },
                               { PoiType::Canyon, 14.0f }, // the gorge
                               { PoiType::Cirque, base > 650.0f ? 15.0f
                                                                : 0.0f },
                               { PoiType::Tarn, base > 650.0f ? 10.0f
                                                              : 0.0f } };
        return pick(t, 7, r);
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
        const Weighted t[] = { { PoiType::Butte, 22.0f },
                               { PoiType::Ridge, 22.0f },
                               { PoiType::Mesa, 13.0f },
                               { PoiType::Waterfall, 15.0f },
                               { PoiType::Canyon, 10.0f },
                               { PoiType::PlainLake, 10.0f },
                               { PoiType::Grove, arid ? 0.0f : 8.0f },
                               { PoiType::Oasis, arid ? 10.0f : 0.0f } };
        return pick(t, 8, r);
    }
    const Weighted t[] = { { PoiType::Butte, 26.0f },
                           { PoiType::PlainLake, 16.0f },
                           { PoiType::Ridge, 10.0f },
                           { PoiType::Mesa, 8.0f },
                           { PoiType::Waterfall, 10.0f }, // a step, rapids
                           { PoiType::Confluence, 8.0f },
                           { PoiType::Grove, arid ? 0.0f : 8.0f },
                           { PoiType::LoneTree, 8.0f },
                           { PoiType::CityPad, 8.0f },
                           { PoiType::Oasis, arid ? 10.0f : 0.0f } };
    return pick(t, 10, r);
}

bool basinType(PoiType type) {
    return type == PoiType::PlainLake || type == PoiType::Tarn ||
           type == PoiType::Cove;
}

bool waterCourseType(PoiType type) {
    return type == PoiType::Waterfall || type == PoiType::Canyon ||
           type == PoiType::Confluence;
}

// The nearest master course to (x, z), read on the PLAN-FREE analytic
// (rhythm.plan off: the network must not read the plan it types for —
// the memo keys on the whole params, so this network is its own).
using CourseHit = PoiCourseHit;

} // namespace

PoiCourseHit poiNearestCourse(const WorldLayerParams& world,
                              const PoiPlanParams& plan, f32 x, f32 z,
                              f32 reach) {
    ProceduralControlParams pc;
    pc.seed = world.seed;
    pc.world = world;
    pc.poi = plan;
    pc.rhythm.plan = false;
    const ProceduralControls controls { pc };
    const MacroParams macro;
    MasterNetworkParams net;
    net.seaLevel = macro.seaLevel;
    // Down to the riviere tier: a waterfall or a gorge belongs to a
    // real river, not only to the rare fleuves (the memo keys on the
    // params, so this finer network lives beside the imprint's).
    net.fleuveArea = 1.5e6f;
    const auto rivers = masterRiversNear(controls, macro, net, x - reach,
                                         z - reach, x + reach, z + reach);
    CourseHit hit;
    for (const MasterRiver& river : rivers) {
        for (size_t k = 0; k < river.nodes.size(); ++k) {
            const MasterNode& n = river.nodes[k];
            const f32 d = std::hypot(n.x - x, n.z - z);
            if (d < hit.dist) {
                hit.dist = d;
                hit.found = true;
                hit.x = n.x;
                hit.z = n.z;
                const MasterNode& a = river.nodes[k > 0 ? k - 1 : k];
                const MasterNode& b =
                    river.nodes[k + 1 < river.nodes.size() ? k + 1 : k];
                const f32 dx = b.x - a.x;
                const f32 dz = b.z - a.z;
                const f32 len = std::hypot(dx, dz);
                if (len > 1.0f) {
                    hit.dirX = dx / len;
                    hit.dirZ = dz / len;
                }
            }
        }
    }
    return hit;
}

namespace {

CourseHit nearestCourse(const WorldLayerParams& world, const PoiPlanParams& plan,
                        f32 x, f32 z, f32 reach) {
    return poiNearestCourse(world, plan, x, z, reach);
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
    s.theta = roll01(s.hash, 6) * 3.14159265f;
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
    } else if (s.type == PoiType::Cirque) {
        // The rim's own height; the kernel digs the tarn from it.
        s.height = lerpHash(120.0f, 200.0f, s.hash, 4);
        s.radius = lerpHash(300.0f, 600.0f, s.hash, 3);
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

// The memo is only ever CLEARED at a public entry point (memoReady):
// the internal functions hold references into it across nested calls.
Memo& memoFor(const WorldLayerParams&, const PoiPlanParams&) {
    thread_local Memo memo;
    return memo;
}

Memo& memoReady(const WorldLayerParams& world, const PoiPlanParams& plan) {
    Memo& memo = memoFor(world, plan);
    const u32 key = paramsKeyOf(world, plan);
    if (memo.paramsKey != key || memo.sites.size() > 32768) {
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
        // Rule F5: a waterfall, a canyon or a confluence needs a
        // master course under it — re-rolled otherwise, then the dry
        // fallback of its family; a kept one snaps onto the course and
        // takes its direction (the canyon runs along, the step across,
        // the uphill side upstream).
        if (tier == PoiTier::Moyen && waterCourseType(s.type)) {
            CourseHit hit = nearestCourse(world, plan, s.x, s.z,
                                          plan.waterPoiReach);
            for (u32 tries = 0; tries < 3 && waterCourseType(s.type) &&
                                !(hit.found && hit.dist <= plan.waterPoiReach);
                 ++tries) {
                s.type = drawType(tier, w, startDisc, s.hash, ++alt);
            }
            if (waterCourseType(s.type)) {
                if (hit.found && hit.dist <= plan.waterPoiReach) {
                    s.x = hit.x;
                    s.z = hit.z;
                    s.theta = s.type == PoiType::Canyon
                                  ? std::atan2(hit.dirZ, hit.dirX)
                                  : std::atan2(hit.dirX, -hit.dirZ);
                } else {
                    s.type = s.type == PoiType::Waterfall  ? PoiType::Butte
                             : s.type == PoiType::Canyon   ? PoiType::Ridge
                                                           : PoiType::LoneTree;
                }
            }
        }
        sizeSite(plan, s);
    }
    return memo.sites.emplace(key, cell).first->second;
}

// --- Kernels ---------------------------------------------------------

struct KernelOut {
    f32 lift { 0.0f };
    f32 basin { 0.0f };
    f32 mesaTop { 0.0f };
    f32 flank { 0.0f };
    f32 padFlat { 0.0f };
};

// Elliptic normalized distance in the site's hashed frame.
struct Frame {
    f32 u, v; // normalized by radius (and aspect along u)
    f32 n;    // sqrt(u^2 + v^2)
};

Frame frameOf(const PoiSite& s, f32 radius, f32 aspect, f32 x, f32 z) {
    const f32 ct = std::cos(s.theta);
    const f32 st = std::sin(s.theta);
    const f32 rx = x - s.x;
    const f32 rz = z - s.z;
    Frame f;
    f.u = (ct * rx + st * rz) / (radius * aspect);
    f.v = (-st * rx + ct * rz) / radius;
    f.n = std::sqrt(f.u * f.u + f.v * f.v);
    return f;
}

f32 ss(f32 lo, f32 hi, f32 x) { return noise::smoothstep01(lo, hi, x); }

// The cone: a linear flank (the triangle silhouette) at a hashed slope,
// its footprint derived from the height — never a dome.
f32 coneRadius(const PoiPlanParams& plan, const PoiSite& s, f32 height) {
    const f32 deg = glm::mix(plan.coneSlopeMinDeg, plan.coneSlopeMaxDeg,
                             roll01(s.hash, 7));
    return glm::max(height / std::tan(deg * 0.017453292f), 20.0f);
}

KernelOut kernelAt(const WorldLayerParams& world, const PoiPlanParams& plan,
                   const PoiSite& s, f32 x, f32 z) {
    KernelOut k;
    const f32 h = glm::max(s.height, 0.0f);
    const f32 depth = glm::max(-s.height, 0.0f);
    switch (s.type) {
    case PoiType::Summit:
    case PoiType::Needle:
    case PoiType::Butte:
    case PoiType::Headland:
    case PoiType::Islet:
    case PoiType::SeaStack: {
        const f32 r = s.type == PoiType::Needle
                          ? glm::max(h / std::tan(0.7f), 20.0f) // ~40 deg
                          : coneRadius(plan, s, h);
        const f32 aspect = s.type == PoiType::Headland ? 2.2f : 1.0f;
        const Frame f = frameOf(s, r, aspect, x, z);
        if (f.n < 1.0f) {
            // Rounded tip (C1 over the last 8 %), linear flank below.
            const f32 t = 1.0f - f.n;
            const f32 tip = 0.08f;
            const f32 kk = t > 1.0f - tip
                               ? 1.0f - (1.0f - t) * (1.0f - t) / (2.0f * tip) -
                                     tip * 0.5f
                               : t;
            k.lift = h * kk;
            k.flank = ss(0.05f, 0.3f, kk) * (1.0f - ss(0.85f, 1.0f, kk));
        }
        break;
    }
    case PoiType::Crater: {
        const f32 r = coneRadius(plan, s, h);
        const Frame f = frameOf(s, r, 1.0f, x, z);
        if (f.n < 1.0f) {
            const f32 kk = glm::min(1.0f, (1.0f - f.n) / 0.85f);
            k.lift = h * kk;
            k.mesaTop = 1.0f - ss(0.1f, 0.2f, f.n);
            // The crater: a bowl in the truncated top.
            k.basin = 0.35f * h * (1.0f - ss(0.05f, 0.18f, f.n));
            k.flank = ss(0.2f, 0.9f, f.n);
        }
        break;
    }
    case PoiType::Mesa: {
        const Frame f = frameOf(s, s.radius, 1.0f + 0.4f * roll01(s.hash, 8),
                                x, z);
        if (f.n < 1.0f) {
            const f32 hm = glm::min(h, 150.0f);
            k.lift = hm * glm::min(1.0f, (1.0f - f.n) / 0.35f);
            k.mesaTop = 1.0f - ss(0.55f, 0.68f, f.n);
            k.flank = ss(0.65f, 0.75f, f.n) * (1.0f - ss(0.95f, 1.0f, f.n));
        }
        break;
    }
    case PoiType::Ridge:
    case PoiType::Col: {
        const f32 aspect = glm::mix(2.8f, 4.5f, roll01(s.hash, 8));
        const Frame f = frameOf(s, s.radius, aspect, x, z);
        if (f.n < 1.0f) {
            const f32 kk = 1.0f - f.n;
            f32 mod = 1.0f;
            if (s.type == PoiType::Ridge) {
                mod = glm::mix(0.55f, 1.0f,
                               noise::fbm(s.hash ^ 0x51d9ec01u, x, z,
                                          1.0f / 1300.0f, 2, 2.0f, 0.5f));
            } else {
                // A col: the saddle is the point, the ridge rises on
                // both sides along the axis.
                mod = 0.4f + 0.6f * ss(0.15f, 0.6f, std::abs(f.u));
            }
            k.lift = h * kk * mod;
            k.flank = ss(0.05f, 0.4f, kk) * (1.0f - ss(0.6f, 0.9f, kk));
        }
        break;
    }
    case PoiType::Escarpment:
    case PoiType::SeaCliff:
    case PoiType::Waterfall: {
        // A step across the footprint: the uphill half-plane rises.
        const f32 r = s.radius * 2.0f;
        const Frame f = frameOf(s, r, 1.0f, x, z);
        if (f.n < 1.0f) {
            const f32 hs = s.type == PoiType::Waterfall ? glm::min(h, 60.0f)
                                                        : glm::min(h, 120.0f);
            const f32 edge = 1.0f - ss(0.6f, 1.0f, f.n);
            k.lift = hs * ss(-0.12f, 0.12f, f.v) * edge;
            k.flank = (1.0f - ss(0.0f, 0.15f, std::abs(f.v))) * edge;
        }
        break;
    }
    case PoiType::Canyon:
    case PoiType::Fjord: {
        const f32 aspect = glm::mix(3.0f, 5.0f, roll01(s.hash, 8));
        const Frame f = frameOf(s, s.radius, aspect, x, z);
        if (f.n < 1.0f) {
            const f32 d = glm::max(h, 60.0f);
            k.basin = d * glm::min(1.0f, (1.0f - f.n) / 0.3f);
            k.flank = ss(0.6f, 0.75f, f.n) * (1.0f - ss(0.95f, 1.0f, f.n));
        }
        break;
    }
    case PoiType::Cirque: {
        // An amphitheatre: a rim on three sides, open along +u.
        const Frame f = frameOf(s, s.radius, 1.0f, x, z);
        if (f.n < 1.0f) {
            const f32 opening = ss(0.3f, 0.8f, f.u) * (1.0f - ss(0.5f, 1.0f, std::abs(f.v)));
            const f32 rim = ss(0.45f, 0.75f, f.n) * (1.0f - ss(0.9f, 1.0f, f.n));
            k.lift = h * rim * (1.0f - opening);
            k.basin = 0.25f * h * (1.0f - ss(0.2f, 0.5f, f.n)); // the tarn
            k.flank = rim;
        }
        break;
    }
    case PoiType::PlainLake:
    case PoiType::Tarn:
    case PoiType::Cove: {
        const Frame f = frameOf(s, s.radius, 1.0f + 0.5f * roll01(s.hash, 8),
                                x, z);
        if (f.n < 1.0f) {
            k.basin = depth * glm::min(1.0f, (1.0f - f.n * f.n) / 0.4f);
        }
        break;
    }
    case PoiType::Hoodoos:
    case PoiType::KarstTowers:
    case PoiType::Boulders: {
        // A field of small cones on a jittered lattice inside the
        // footprint (towers are tall and thin, boulders squat).
        const f32 cell = s.type == PoiType::Boulders ? 25.0f
                         : s.type == PoiType::Hoodoos ? 45.0f
                                                      : 90.0f;
        const f32 hc = s.type == PoiType::Boulders ? 6.0f
                       : s.type == PoiType::Hoodoos ? 20.0f
                                                    : 60.0f;
        const Frame f = frameOf(s, s.radius, 1.0f, x, z);
        if (f.n < 1.0f) {
            const i32 cx = static_cast<i32>(std::floor(x / cell));
            const i32 cz = static_cast<i32>(std::floor(z / cell));
            f32 best = 0.0f;
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    const u32 salt = s.hash ^ 0x7007e25u;
                    const f32 px = (static_cast<f32>(cx + dx) +
                                    noise::lattice(salt, cx + dx, cz + dz)) *
                                   cell;
                    const f32 pz = (static_cast<f32>(cz + dz) +
                                    noise::lattice(salt + 1u, cx + dx, cz + dz)) *
                                   cell;
                    const f32 rr = cell * 0.45f;
                    const f32 d = std::hypot(x - px, z - pz) / rr;
                    if (d < 1.0f) {
                        best = glm::max(best, hc * (1.0f - d));
                    }
                }
            }
            k.lift = best * (1.0f - ss(0.8f, 1.0f, f.n));
            k.flank = k.lift > 1.0f ? 0.5f : 0.0f;
        }
        break;
    }
    case PoiType::Grove:
    case PoiType::LoneTree:
    case PoiType::Spring:
    case PoiType::Oasis: {
        const Frame f = frameOf(s, s.radius, 1.0f, x, z);
        if (f.n < 1.0f) {
            const f32 kk = (1.0f - f.n * f.n) * (1.0f - f.n * f.n);
            if (s.type == PoiType::Oasis) {
                k.basin = 3.0f * kk;
            } else if (s.type == PoiType::Spring) {
                k.basin = 0.0f; // a marker: the water comes with the peuplement
            } else {
                k.lift = glm::min(h, 12.0f) * kk;
            }
        }
        break;
    }
    case PoiType::CityPad:
    case PoiType::Confluence: {
        const Frame f = frameOf(s, s.radius, 1.0f, x, z);
        if (f.n < 1.0f) {
            k.padFlat = 1.0f - ss(0.7f, 1.0f, f.n);
        }
        break;
    }
    default:
        break;
    }
    (void)world;
    return k;
}

// Distance to a segment and the parameter along it.
f32 segmentDistance(f32 px, f32 pz, f32 ax, f32 az, f32 bx, f32 bz,
                    f32& t) {
    const f32 dx = bx - ax;
    const f32 dz = bz - az;
    const f32 len2 = dx * dx + dz * dz;
    t = len2 > 0.0f
            ? glm::clamp(((px - ax) * dx + (pz - az) * dz) / len2, 0.0f, 1.0f)
            : 0.0f;
    return std::hypot(px - (ax + dx * t), pz - (az + dz * t));
}

} // namespace

namespace {

struct CharacterStyle {
    f32 reliefMul, wavelengthMul, wetBias, hardBias, coverBias;
};

// docs/POI-CATALOGUE.md §E, the temperate column (the other biomes
// reuse the styles; their palettes come from the climate).
constexpr CharacterStyle kCharacters[] = {
    { 1.0f, 1.0f, 0.0f, 0.0f, 0.0f },     // rolling meadow
    { 0.8f, 0.7f, 0.1f, 0.0f, 0.0f },     // bocage
    { 1.25f, 0.85f, 0.0f, 0.0f, 0.0f },   // wooded hills
    { 0.4f, 1.2f, 0.6f, -0.1f, 0.2f },    // marsh
    { 0.7f, 1.5f, -0.2f, 0.35f, -0.1f },  // rocky plateau
    { 0.9f, 1.8f, -0.1f, 0.1f, 0.3f },    // heath
};

} // namespace

PlanSample planSampleAt(const WorldLayerParams& world,
                        const PoiPlanParams& plan, f32 x, f32 z) {
    PlanSample out;
    memoReady(world, plan);
    // Sites: the 3x3 moyen and petit cells, the grand cells within reach
    // (a grand's cone reaches ~1 km: its own cell and the neighbours).
    f32 nearestD = 1.0e30f;
    // The three nearest moyen/grand sites for the character blend.
    f32 nearD[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
    u8 nearC[3] = { 0, 0, 0 };
    for (const PoiTier tier : { PoiTier::Grand, PoiTier::Moyen, PoiTier::Petit }) {
        const f32 size = cellSize(plan, tier);
        const i32 cx = static_cast<i32>(std::floor(x / size));
        const i32 cz = static_cast<i32>(std::floor(z / size));
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const CellSite& c = typedSite(world, plan, tier, cx + dx, cz + dz);
                if (!c.present) {
                    continue;
                }
                const PoiSite& s = c.site;
                const f32 d = std::hypot(x - s.x, z - s.z);
                if (tier != PoiTier::Petit) {
                    if (d < nearestD) {
                        nearestD = d;
                        out.character = static_cast<u8>(s.hash % 6u);
                    }
                    const u8 c = static_cast<u8>(s.hash % 6u);
                    for (u32 k = 0; k < 3; ++k) {
                        if (d < nearD[k]) {
                            for (u32 j = 2; j > k; --j) {
                                nearD[j] = nearD[j - 1];
                                nearC[j] = nearC[j - 1];
                            }
                            nearD[k] = d;
                            nearC[k] = c;
                            break;
                        }
                    }
                }
                // Footprint reject before any kernel (cones derive their
                // footprint from the height: bound by the max slope).
                const f32 reach = glm::max(
                    s.radius * 2.2f,
                    glm::max(s.height, 0.0f) / std::tan(plan.coneSlopeMinDeg *
                                                        0.017453292f));
                if (d > reach) {
                    continue;
                }
                const KernelOut k = kernelAt(world, plan, s, x, z);
                out.lift = glm::max(out.lift, k.lift);
                out.basin = glm::max(out.basin, k.basin);
                out.mesaTop = glm::max(out.mesaTop, k.mesaTop);
                out.flank = glm::max(out.flank, k.flank);
                out.padFlat = glm::max(out.padFlat, k.padFlat);
            }
        }
    }
    // The character blend: inverse-square weights of the three nearest
    // sites (a ~250 m fade at a Voronoi border).
    {
        f32 wsum = 0.0f;
        CharacterStyle mix { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        for (u32 k = 0; k < 3; ++k) {
            if (nearD[k] >= 1.0e29f) {
                continue;
            }
            const f32 w = 1.0f / (nearD[k] * nearD[k] + 150.0f * 150.0f);
            const CharacterStyle& c = kCharacters[nearC[k]];
            mix.reliefMul += w * c.reliefMul;
            mix.wavelengthMul += w * c.wavelengthMul;
            mix.wetBias += w * c.wetBias;
            mix.hardBias += w * c.hardBias;
            mix.coverBias += w * c.coverBias;
            wsum += w;
        }
        if (wsum > 0.0f) {
            out.reliefMul = mix.reliefMul / wsum;
            out.wavelengthMul = mix.wavelengthMul / wsum;
            out.wetBias = mix.wetBias / wsum;
            out.hardBias = mix.hardBias / wsum;
            out.coverBias = mix.coverBias / wsum;
        }
    }
    // Walks: corridors and screens of the edges around.
    {
        const f32 size = plan.moyenCell;
        const i32 reachCells = static_cast<i32>(std::ceil(plan.edgeReach / size));
        const i32 cx = static_cast<i32>(std::floor(x / size));
        const i32 cz = static_cast<i32>(std::floor(z / size));
        f32 screenLift = 0.0f;
        for (i32 dz = -reachCells; dz <= reachCells; ++dz) {
            for (i32 dx = -reachCells; dx <= reachCells; ++dx) {
                const CellSite& self = typedSite(world, plan, PoiTier::Moyen,
                                                 cx + dx, cz + dz);
                if (!self.present) {
                    continue;
                }
                for (const PoiSite& t : rawEdgesOf(world, plan, cx + dx, cz + dz)) {
                    if (!lowerRank(cx + dx, cz + dz, t.cellX, t.cellZ)) {
                        continue; // canonical endpoint only
                    }
                    bool mutual = false;
                    for (const PoiSite& back : rawEdgesOf(world, plan, t.cellX, t.cellZ)) {
                        if (back.cellX == cx + dx && back.cellZ == cz + dz) {
                            mutual = true;
                            break;
                        }
                    }
                    if (!mutual) {
                        continue;
                    }
                    const PoiSite& a = self.site;
                    const u32 eh = core::hashU32(a.hash ^ (t.hash * 0x27d4eb2fu));
                    const f32 ex = t.x - a.x;
                    const f32 ez = t.z - a.z;
                    const f32 len = std::hypot(ex, ez);
                    if (len < 1.0f) {
                        continue;
                    }
                    const f32 bend = glm::mix(-0.25f, 0.25f, roll01(eh, 1));
                    const f32 wx = (a.x + t.x) * 0.5f - ez / len * bend * len;
                    const f32 wz = (a.z + t.z) * 0.5f + ex / len * bend * len;
                    // Corridor: the nearer of the two segments.
                    f32 t1, t2;
                    const f32 d1 = segmentDistance(x, z, a.x, a.z, wx, wz, t1);
                    const f32 d2 = segmentDistance(x, z, wx, wz, t.x, t.z, t2);
                    const f32 half = glm::mix(plan.corridorHalfWidthMin,
                                              plan.corridorHalfWidthMax,
                                              roll01(eh, 2));
                    const f32 d = glm::min(d1, d2);
                    const f32 corridor = 1.0f - ss(half, half * 1.6f, d);
                    out.corridor = glm::max(out.corridor, corridor);
                    // Screen: a transverse ridge at 45-60 % of the walk,
                    // notched on the corridor (the col the walk crosses).
                    const f32 at = glm::mix(0.45f, 0.6f, roll01(eh, 3));
                    const f32 l1 = std::hypot(wx - a.x, wz - a.z);
                    const f32 total = l1 + std::hypot(t.x - wx, t.z - wz);
                    const f32 along = at * total;
                    f32 sx, sz, dirx, dirz;
                    if (along <= l1) {
                        const f32 u = along / glm::max(l1, 1.0f);
                        sx = glm::mix(a.x, wx, u);
                        sz = glm::mix(a.z, wz, u);
                        dirx = (wx - a.x) / glm::max(l1, 1.0f);
                        dirz = (wz - a.z) / glm::max(l1, 1.0f);
                    } else {
                        const f32 l2 = glm::max(total - l1, 1.0f);
                        const f32 u = (along - l1) / l2;
                        sx = glm::mix(wx, t.x, u);
                        sz = glm::mix(wz, t.z, u);
                        dirx = (t.x - wx) / l2;
                        dirz = (t.z - wz) / l2;
                    }
                    const f32 halfLen = glm::mix(plan.screenHalfLengthMin,
                                                 plan.screenHalfLengthMax,
                                                 roll01(eh, 4));
                    const f32 hs = glm::mix(plan.screenHeightMin,
                                            plan.screenHeightMax, roll01(eh, 5));
                    // Frame: u along the screen (perpendicular to the
                    // walk), v across it (along the walk).
                    const f32 rx = x - sx;
                    const f32 rz = z - sz;
                    const f32 su = (-dirz * rx + dirx * rz) / halfLen;
                    const f32 sv = (dirx * rx + dirz * rz) / plan.screenHalfWidth;
                    const f32 n = std::sqrt(su * su + sv * sv);
                    if (n < 1.0f) {
                        const f32 notch = 1.0f - plan.screenNotch * corridor;
                        screenLift = glm::max(screenLift, hs * (1.0f - n) * notch);
                    }
                }
            }
        }
        out.lift = glm::max(out.lift, screenLift);
    }
    return out;
}

u8 characterPalette(u8 character) {
    switch (static_cast<PoiCharacter>(character % 6u)) {
    case PoiCharacter::WoodedHills: return 6;
    case PoiCharacter::Bocage: return 7;
    case PoiCharacter::Marsh: return 9;
    default: return 0; // meadow, rocky plateau, heath: cover variants
    }
}

PlanCharacter planCharacterAt(const WorldLayerParams& world,
                              const PoiPlanParams& plan, f32 x, f32 z) {
    PlanCharacter out;
    memoReady(world, plan);
    f32 nearD[3] = { 1.0e30f, 1.0e30f, 1.0e30f };
    u8 nearC[3] = { 0, 0, 0 };
    for (const PoiTier tier : { PoiTier::Grand, PoiTier::Moyen }) {
        const f32 size = cellSize(plan, tier);
        const i32 cx = static_cast<i32>(std::floor(x / size));
        const i32 cz = static_cast<i32>(std::floor(z / size));
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const CellSite& c = typedSite(world, plan, tier, cx + dx, cz + dz);
                if (!c.present) {
                    continue;
                }
                const f32 d = std::hypot(x - c.site.x, z - c.site.z);
                const u8 ch = static_cast<u8>(c.site.hash % 6u);
                for (u32 k = 0; k < 3; ++k) {
                    if (d < nearD[k]) {
                        for (u32 j = 2; j > k; --j) {
                            nearD[j] = nearD[j - 1];
                            nearC[j] = nearC[j - 1];
                        }
                        nearD[k] = d;
                        nearC[k] = ch;
                        break;
                    }
                }
            }
        }
    }
    f32 wsum = 0.0f, bias = 0.0f;
    for (u32 k = 0; k < 3; ++k) {
        if (nearD[k] >= 1.0e29f) {
            continue;
        }
        const f32 w = 1.0f / (nearD[k] * nearD[k] + 150.0f * 150.0f);
        bias += w * kCharacters[nearC[k]].coverBias;
        wsum += w;
    }
    out.character = nearC[0];
    out.coverBias = wsum > 0.0f ? bias / wsum : 0.0f;
    return out;
}

vector<PoiSite> poiSitesNear(const WorldLayerParams& world,
                             const PoiPlanParams& plan, f32 minX, f32 minZ,
                             f32 maxX, f32 maxZ) {
    vector<PoiSite> out;
    memoReady(world, plan);
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
    memoReady(world, plan);
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
