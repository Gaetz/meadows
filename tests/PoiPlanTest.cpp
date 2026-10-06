#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>

#include "engine/terrain/generation/PoiPlan.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// The POI plan (docs/POI-CATALOGUE.md): world-anchored, deterministic,
// identical from either side of a map line; the densities and the
// variety rules the catalogue promises.

using namespace render::terraingen;

namespace {

ProceduralControlParams paramsFor(u32 seed) {
    ProceduralControlParams pc;
    pc.seed = seed;
    const ProceduralControls controls { pc }; // syncs world.seed
    return controls.params();
}

u64 siteKey(const PoiSite& s) {
    return (static_cast<u64>(static_cast<u32>(s.cellX)) << 34) ^
           (static_cast<u64>(static_cast<u32>(s.cellZ)) << 2) ^
           static_cast<u64>(s.tier);
}

u64 edgeKey(const PoiEdge& e) {
    const u64 a = siteKey(e.a);
    const u64 b = siteKey(e.b);
    return (a < b ? a : b) * 0x9e3779b97f4a7c15ull + (a < b ? b : a);
}

} // namespace

TEST_CASE("poi plan: deterministic and typed") {
    const ProceduralControlParams pc = paramsFor(1337);
    const auto a = poiSitesNear(pc.world, pc.poi, 0.0f, 0.0f, 8192.0f, 8192.0f);
    const auto b = poiSitesNear(pc.world, pc.poi, 0.0f, 0.0f, 8192.0f, 8192.0f);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].type == b[i].type);
        CHECK(a[i].height == b[i].height);
        CHECK(static_cast<u8>(a[i].type) < static_cast<u8>(PoiType::Count));
        CHECK(a[i].radius > 0.0f);
    }
    // Another seed moves them.
    const ProceduralControlParams other = paramsFor(42);
    const auto c = poiSitesNear(other.world, other.poi, 0.0f, 0.0f, 8192.0f, 8192.0f);
    bool moved = c.size() != a.size();
    for (size_t i = 0; !moved && i < a.size(); ++i) {
        moved = a[i].x != c[i].x;
    }
    CHECK(moved);
}

TEST_CASE("poi plan: densities per 8 km map") {
    // Over 16 maps: one grand each, 16 moyens each (one per 2 km cell),
    // moyens never closer than a kilometer, a few hundred petits.
    const ProceduralControlParams pc = paramsFor(1337);
    u32 grands = 0, moyens = 0, petits = 0;
    f32 closest = 1.0e9f;
    for (i32 mz = -2; mz < 2; ++mz) {
        for (i32 mx = -2; mx < 2; ++mx) {
            const f32 x0 = static_cast<f32>(mx) * 8192.0f;
            const f32 z0 = static_cast<f32>(mz) * 8192.0f;
            const auto sites =
                poiSitesNear(pc.world, pc.poi, x0, z0, x0 + 8192.0f, z0 + 8192.0f);
            u32 g = 0, m = 0, p = 0;
            vector<const PoiSite*> ms;
            for (const PoiSite& s : sites) {
                if (s.tier == PoiTier::Grand) {
                    ++g;
                } else if (s.tier == PoiTier::Moyen) {
                    ++m;
                    ms.push_back(&s);
                } else {
                    ++p;
                }
            }
            CHECK(g == 1);
            CHECK(m >= 14);
            CHECK(m <= 18);
            for (size_t i = 0; i < ms.size(); ++i) {
                for (size_t j = i + 1; j < ms.size(); ++j) {
                    closest = std::min(
                        closest, std::hypot(ms[i]->x - ms[j]->x,
                                            ms[i]->z - ms[j]->z));
                }
            }
            grands += g;
            moyens += m;
            petits += p;
        }
    }
    MESSAGE("16 maps: grands ", grands, ", moyens ", moyens, ", petits ",
            petits, ", closest moyen pair ", closest, " m");
    CHECK(closest >= 1000.0f);
    CHECK(petits >= 16 * 150);
}

TEST_CASE("poi plan: symmetric at a map line") {
    // Sites and edges straddling x = 8192 computed from the west map's
    // rect and from the east map's rect are the SAME objects: the plan
    // has no per-map state.
    const ProceduralControlParams pc = paramsFor(1337);
    const auto west = poiEdgesNear(pc.world, pc.poi, 0.0f, 0.0f, 8192.0f, 8192.0f);
    const auto east = poiEdgesNear(pc.world, pc.poi, 8192.0f, 0.0f, 16384.0f, 8192.0f);
    std::map<u64, PoiEdge> byKey;
    for (const PoiEdge& e : west) {
        byKey[edgeKey(e)] = e;
    }
    u32 shared = 0;
    for (const PoiEdge& e : east) {
        const auto it = byKey.find(edgeKey(e));
        const bool straddles = (e.a.x < 8192.0f) != (e.b.x < 8192.0f);
        if (!straddles) {
            continue;
        }
        REQUIRE(it != byKey.end()); // the west map knows this crossing walk
        CHECK(it->second.wx == e.wx);
        CHECK(it->second.a.type == e.a.type);
        CHECK(it->second.b.type == e.b.type);
        ++shared;
    }
    MESSAGE("walks crossing x = 8192: ", shared);
    CHECK(shared >= 1);
    // Each unordered pair appears once per query.
    std::map<u64, u32> counts;
    for (const PoiEdge& e : west) {
        ++counts[edgeKey(e)];
    }
    for (const auto& [k, n] : counts) {
        CHECK(n == 1);
    }
}

TEST_CASE("poi plan: the graph links every moyen, never two alike adjacent") {
    const ProceduralControlParams pc = paramsFor(1337);
    const auto sites = poiSitesNear(pc.world, pc.poi, -8192.0f, -8192.0f,
                                    16384.0f, 16384.0f);
    const auto edges = poiEdgesNear(pc.world, pc.poi, -8192.0f, -8192.0f,
                                    16384.0f, 16384.0f);
    std::map<u64, u32> degree;
    u32 alike = 0;
    for (const PoiEdge& e : edges) {
        ++degree[siteKey(e.a)];
        ++degree[siteKey(e.b)];
        alike += e.a.type == e.b.type;
        CHECK(std::hypot(e.a.x - e.b.x, e.a.z - e.b.z) <= pc.poi.edgeReach);
    }
    u32 moyens = 0, isolated = 0;
    std::map<u8, u32> typeCounts;
    for (const PoiSite& s : sites) {
        if (s.tier != PoiTier::Moyen) {
            continue;
        }
        ++moyens;
        isolated += degree[siteKey(s)] == 0;
        ++typeCounts[static_cast<u8>(s.type)];
    }
    MESSAGE("moyens ", moyens, ", edges ", edges.size(), ", isolated ",
            isolated, ", alike-adjacent ", alike, ", distinct types ",
            typeCounts.size());
    CHECK(isolated == 0);
    CHECK(100 * alike <= 3 * static_cast<u32>(edges.size()));
    CHECK(typeCounts.size() >= 5);
}

TEST_CASE("poi plan: the start cell's grand stands clear of the spawn") {
    for (const u32 seed : { 1u, 42u, 1337u, 2024u }) {
        const ProceduralControlParams pc = paramsFor(seed);
        const auto sites = poiSitesNear(pc.world, pc.poi, 0.0f, 0.0f, 8192.0f, 8192.0f);
        for (const PoiSite& s : sites) {
            if (s.tier == PoiTier::Grand) {
                CHECK(std::hypot(s.x - pc.world.startX, s.z - pc.world.startZ) >=
                      pc.poi.grandStartClearance - 1.0f);
            }
        }
    }
}
