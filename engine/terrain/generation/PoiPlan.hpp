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
    f32 theta { 0.0f };  // the landform's axis (hashed; along the course for water POIs)
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
    // Above the story texture (ridged ranges to ~215 m at 0.8): a
    // moyen stands clear of it, a grand twice over.
    f32 grandHeightMin { 500.0f };
    f32 grandHeightMax { 700.0f };
    f32 moyenHeightMin { 220.0f };
    f32 moyenHeightMax { 340.0f };
    f32 petitHeightMin { 10.0f };
    f32 petitHeightMax { 40.0f };
    f32 edgeReach { 2200.0f }; // moyen-moyen link reach
    u32 edgeMax { 6 };         // per site (the relative-neighbourhood test prunes further)
    // Water POIs (waterfall, canyon, confluence) exist only ON a master
    // course (catalogue rule F5): a site farther than this from a
    // course re-rolls; a kept one snaps onto the course and takes its
    // direction.
    f32 waterPoiReach { 450.0f };
    f32 grandStartClearance { 1500.0f }; // the start cell's grand vs the spawn
    // Kernels: cone flanks (the triangle silhouette), the walks' tubes
    // and the screens that hide then reveal (docs/POI-CATALOGUE.md §G).
    // 18-28 deg: a 200 m moyen spans 400-600 m of base, a landmark
    // read from the next one, not a spike (needles keep their 40 deg).
    f32 coneSlopeMinDeg { 18.0f };
    f32 coneSlopeMaxDeg { 28.0f };
    // Verticality — the Breath of the Wild / Elden Ring read: a
    // landscape of cliffs and benches, not of slopes. 0 = the soft
    // look (cones at 18-28 deg, no terracing), 1 = full: the cones
    // take the steep slopes below, the mesa and step rims sharpen,
    // and the characters terrace the ground into walls
    // (ControlSample::terrace, landHeight's cliffStep).
    f32 verticality { 1.0f };
    f32 coneSlopeSteepMinDeg { 30.0f };
    f32 coneSlopeSteepMaxDeg { 42.0f };
    f32 corridorHalfWidthMin { 60.0f };
    f32 corridorHalfWidthMax { 120.0f };
    f32 screenHeightMin { 40.0f };
    f32 screenHeightMax { 90.0f };
    f32 screenHalfLengthMin { 125.0f };
    f32 screenHalfLengthMax { 250.0f };
    f32 screenHalfWidth { 90.0f };
    f32 screenNotch { 0.75f }; // fraction of the screen cut on the corridor
};

// What the terrain reads at (x, z): the landform lift of the sites (the
// max of their kernels; < 0 inside a basin), the walks' corridors and
// screens, the pads, and the character region (P3 reads it).
struct PlanSample {
    f32 lift { 0.0f };       // meters of base lift (positive kernels)
    f32 basin { 0.0f };      // meters of depression (basin kernels), >= 0
    f32 mesaTop { 0.0f };    // [0,1] on a flat top (mesa, crater rim)
    f32 flank { 0.0f };      // [0,1] on a cone's flank
    f32 corridor { 0.0f };   // [0,1] inside a walk's tube
    f32 padFlat { 0.0f };    // [0,1] inside a site pad
    // The CHARACTER region (docs/POI-CATALOGUE.md §E): the nearest
    // moyen/grand site owns a character; the multipliers blend over
    // the three nearest (1/d^2) so nothing steps at a border.
    u8 character { 0 };      // the dominant character id
    f32 reliefMul { 1.0f };  // tier relief amplitude multiplier
    f32 wavelengthMul { 1.0f }; // tier relief wavelength multiplier
    f32 wetBias { 0.0f };    // beds deeper, cover wetter
    f32 hardBias { 0.0f };   // lithology
    f32 coverBias { 0.0f };  // pushes the cover selector (heath/dry)
    // [0,1] cliff-and-bench quantization strength: the character's
    // terracing and the cones' flanks, zero across a corridor or a pad.
    f32 terrace { 0.0f };
};

// The character table: six landscape styles between the points of
// interest, indexed by a site's hash.
enum class PoiCharacter : u8 {
    RollingMeadow = 0, // the reference: relief x1, lambda x1
    Bocage,            // small close bumps, a little wetter
    WoodedHills,       // ample hills (the scatter reads the cover)
    Marsh,             // nearly flat, wet, long waves
    RockyPlateau,      // mesa country: low relief, long waves, hard
    Heath,             // bare rolling land, long waves, heath cover
    Count
};
const char* poiCharacterName(PoiCharacter c);
PlanSample planSampleAt(const WorldLayerParams& world,
                        const PoiPlanParams& plan, f32 x, f32 z);
// The character region alone (no kernels): the dominant character and
// the blended cover bias — the per-texel palette id reads them.
struct PlanCharacter {
    u8 character { 0 };
    f32 coverBias { 0.0f };
};
PlanCharacter planCharacterAt(const WorldLayerParams& world,
                              const PoiPlanParams& plan, f32 x, f32 z);
// The temperate palette a character names (0 = none: the climate's own).
u8 characterPalette(u8 character);
// The nearest fine master course (riviere tier) to a point, on the
// plan-free analytic — what rule F5 types the water POIs against.
struct PoiCourseHit {
    bool found { false };
    f32 dist { 1.0e30f };
    f32 x { 0.0f }, z { 0.0f };
    f32 dirX { 1.0f }, dirZ { 0.0f };
};
PoiCourseHit poiNearestCourse(const WorldLayerParams& world,
                              const PoiPlanParams& plan, f32 x, f32 z,
                              f32 reach);

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
