#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/WorldLayer.hpp"

// The POI PLAN (docs/POI-CATALOGUE.md, docs/PAYSAGE.md §7.6): the points
// of interest come FIRST, the terrain is shaped around them. Three tiers
// of sites on three world-anchored jittered lattices (grand: one per
// 8 km cell, the far goal; moyen: one per 2 km cell, the goal of the
// next walk; petit: the detail at fifty meters), typed from the world
// sample at the site through the catalogue's tables, linked by a
// relative-neighbourhood graph of edges (the walks). Pure functions of
// (seed, x, z) memoized per cell: two maps compute the same sites and
// the same edges on both sides of their line.

namespace render::terraingen {

enum class PoiTier : u8 { Grand = 0, Moyen = 1, Petit = 2 };

// The catalogue's types that have (or will have) a landform kernel.
enum class PoiType : u8 {
    Summit = 0,  // A: pyramidal peak
    Needle,      // A: rock tooth
    Ridge,       // A: crest with cols
    Mesa,        // A: flat top, steep rim
    Butte,       // A: dome
    Escarpment,  // A: cliff step
    Canyon,      // A: gorge
    Col,         // A: saddle
    Hoodoos,     // A: field of small cones
    KarstTowers, // A: towers
    Boulders,    // A: boulder field
    Crater,      // A: volcanic cone with crater
    Cirque,      // A: glacial cirque
    Waterfall,   // B
    PlainLake,   // B: basin (negative kernel)
    Tarn,        // B
    Spring,      // B
    Oasis,       // B
    Confluence,  // B: site
    Headland,    // C
    SeaCliff,    // C
    SeaStack,    // C
    Cove,        // C: coastal basin
    Fjord,       // C
    Islet,       // C
    LoneTree,    // D
    Grove,       // D
    CityPad,     // site: flat pad near water
    Count
};

const char* poiTypeName(PoiType type);

struct PoiSite {
    f32 x { 0.0f };
    f32 z { 0.0f };
    PoiTier tier { PoiTier::Moyen };
    PoiType type { PoiType::Butte };
    f32 radius { 0.0f }; // footprint (m)
    f32 height { 0.0f }; // lift at the summit above the floor (m); < 0 = basin
    u32 hash { 0 };      // the site's own salt (variants, satellites)
    i32 cellX { 0 };     // its lattice cell (identity across maps)
    i32 cellZ { 0 };
};

// A walk between two moyen sites, bent through a hashed waypoint.
struct PoiEdge {
    PoiSite a;
    PoiSite b;
    f32 wx { 0.0f }; // waypoint
    f32 wz { 0.0f };
};

struct PoiPlanParams {
    f32 grandCell { 8192.0f }; // the map itself: one grand per map
    f32 moyenCell { 2048.0f };
    f32 petitCell { 350.0f };
    f32 petitChance { 0.6f };
    f32 grandHeightMin { 400.0f };
    f32 grandHeightMax { 600.0f };
    f32 moyenHeightMin { 150.0f };
    f32 moyenHeightMax { 250.0f };
    f32 petitHeightMin { 10.0f };
    f32 petitHeightMax { 40.0f };
    f32 edgeReach { 2200.0f }; // moyen-moyen link reach
    u32 edgeMax { 6 };         // per site (the relative-neighbourhood test prunes further)
    f32 grandStartClearance { 1500.0f }; // the start cell's grand vs the spawn
};

// Sites of every tier whose centre lies in the rect (world meters).
vector<PoiSite> poiSitesNear(const WorldLayerParams& world,
                             const PoiPlanParams& plan, f32 minX, f32 minZ,
                             f32 maxX, f32 maxZ);
// Edges with at least one endpoint in the rect. Each unordered pair
// appears once; the set is identical whatever rect contains the pair.
vector<PoiEdge> poiEdgesNear(const WorldLayerParams& world,
                             const PoiPlanParams& plan, f32 minX, f32 minZ,
                             f32 maxX, f32 maxZ);

} // namespace render::terraingen
