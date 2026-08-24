#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp" // render::kDefaultSeaLevel

// Terrain generation pipeline — stage S1 (macro synthesis). Headless
// (lib meadows): the bake runs on JobSystem workers or in doctests, never
// on the render side. docs/TERRAIN-GEN.md holds the pipeline overview.
//
// The pipeline consumes CONTROL FIELDS (elevation tier, uplift, sea,
// biome) through the ControlSource seam. Two providers exist by design:
// ProceduralControls (sandbox mode — everything derived from the world
// seed) and painted control maps (scenario/editor mode, later brick). The
// stages downstream never know which one fed them.

namespace render::terraingen {

struct GridSpec {
    f32 originX { 0.0f }; // world meters, min corner
    f32 originZ { 0.0f };
    f32 texelSize { 8.0f };
    u32 n { 0 }; // n x n samples

    f32 x(u32 col) const { return originX + static_cast<f32>(col) * texelSize; }
    f32 z(u32 row) const { return originZ + static_cast<f32>(row) * texelSize; }
    size_t cells() const { return static_cast<size_t>(n) * n; }
};

struct ControlSample {
    f32 tier { 0.0f };   // continuous index into MacroParams::tiers
    f32 uplift { 0.0f }; // [0,1] tectonic uplift strength (stage S2 input)
    bool sea { false };  // this point is open water by decree
    u8 biome { 0 };
    // Relief-regime extras (defaults keep painted/test sources legacy):
    f32 plateau { 0.0f };    // extra base altitude (old massifs + swell)
    f32 hillRelief { 0.0f }; // ridged hill-chain relief amplitude (m)
    // Passability corridors [0,1]: 1 = soften the erosion here (soft
    // rock -> gentle equilibrium slopes, no fine ravines) — mountain
    // passes and walkable gaps between hills, drama kept elsewhere.
    f32 gentle { 0.0f };
    // Calm socles [0,1]: the habitable terrain family (true plains,
    // highland plateau tops, corridors) — dissection is damped here so
    // the ground stays walkable and readable. Valley floors join the
    // family post-erosion in the tile bake (they are unknowable
    // pointwise). Superset of `gentle`.
    f32 calm { 0.0f };
    // Multiplier on the OSCILLATING relief carriers (tier relief, hill
    // chains) in landHeight — never on the tier floor or the base
    // lift. Landmark clearings flatten with it; 1 = legacy.
    f32 reliefScale { 1.0f };
    // Valley axis (undirected, mod pi): landHeight stretches the
    // relief carriers along it so crests and valleys elongate into
    // sightlines. Strength 0 = isotropic legacy (the default keeps
    // painted/test control sources unchanged).
    f32 axisCos { 1.0f };
    f32 axisSin { 0.0f };
    f32 axisStrength { 0.0f };
    // Master-valley (trunk) transverse profile: [0,1] floorness (1 on
    // the flat valley floor — future fleuve beds, site scoring) and
    // the depression depth in meters landHeight digs (already faded
    // by profile/inland/axis strength; 0 = none).
    f32 trunk { 0.0f };
    f32 trunkDepth { 0.0f };
    // Lithology [0,1]: rock hardness. Hard rock erodes slow, holds
    // steeper scree, and cliffs into the sea (calanques); soft rock
    // rolls gentle and beaches. 0.5 = neutral (painted/test default).
    f32 hardness { 0.5f };
};

class ControlSource {
public:
    virtual ~ControlSource() = default;
    virtual ControlSample at(f32 x, f32 z) const = 0;
};

// Sandbox controls: continentalness (warped low-frequency FBM) decides
// sea and elevation tier; a ridged mask decides where ranges rise. Pure
// functions of (seed, x, z) — infinite and deterministic.
struct ProceduralControlParams {
    u32 seed { 1337 };
    // Horizontal rhythm, two families with OPPOSITE scales:
    //   LANDFORM CARRIERS (continent, uplift, hill chains, swell, tier
    //   relief) run LONG — same heights spread over wide distances, so
    //   slopes stay gentle and landscapes breathe;
    //   TYPE SELECTORS (regime, climate) run SHORT — a walk of a few
    //   hundred meters changes the landscape/biome character. Variety
    //   comes from the selectors switching character over slow
    //   carriers, not from the carriers themselves oscillating fast.
    //   (gentle sits apart: passage width is a gameplay constant.)
    // The continent wave is the walking rhythm of the world: it paces
    // the tier ramp (plains -> hills -> high country) and the coasts,
    // and its high-tier patches are what let the uplift ranges fire —
    // shrinking it makes full-size massifs RARER without touching their
    // size. Below ~4000 the landmasses crumble into an archipelago.
    f32 continentWavelength { 4000.0f };
    // CONTINENTAL CARRIER (experiment, default off): a very slow field
    // added onto continentalness — it decides WHERE the land masses
    // and the open seas are, while the 4 km fbm keeps drawing the
    // local coastline character. High zones become solid continents
    // (long drainage basins, real fleuves), low zones open ocean —
    // the macro-scale hierarchy the 100 km map showed was missing.
    // 0 = off (bit-identical legacy world).
    f32 continentCarrierWavelength { 0.0f };
    f32 continentCarrierAmp { 0.3f };
    // CONTINENT LAYOUT (design-forced, default off): a jittered mega
    // grid of warped multi-lobe kernels GUARANTEES the world shape —
    // the start sits on a continental island, another continent lies
    // within ~a cell, large islands scatter between. The regional
    // carrier above keeps articulating coasts, straits and inland
    // seas on top; this layer only decides where land masses are.
    bool continentLayout { false };
    f32 continentCellSize { 700000.0f };
    f32 continentRadiusMin { 160000.0f };
    f32 continentRadiusMax { 260000.0f };
    f32 islandCellSize { 280000.0f };
    f32 islandChance { 0.45f };
    f32 islandRadiusMin { 30000.0f };
    f32 islandRadiusMax { 80000.0f };
    f32 layoutAmp { 0.34f }; // continentalness lift inside/-outside
    f32 seaThreshold { 0.42f }; // continentalness below -> open sea
    f32 tierSpread { 0.22f };   // continentalness span of the tier ramp
    f32 maxTier { 3.0f };
    f32 upliftWavelength { 6500.0f };
    f32 upliftMaskLow { 0.38f };
    f32 upliftMaskHigh { 0.68f };
    f32 warpWavelength { 1600.0f }; // continent-shape domain warp
    f32 warpStrength { 440.0f };    // scaled with the continent wave
    // Climate fields deciding the biome id (must match the BiomeForm
    // palette shipped in data: 0 temperate, 1 arid, 2 alpine, 3 tundra).
    f32 climateWavelength { 350.0f };
    // Relief regimes: a type-selector field sorts the land into three
    // characters — HILL-CHAIN country (low, rolling ridged hills, no
    // uplift), OLD MASSIFS (an elevated plateau wearing hills, uplift
    // nearly off — the Massif Central look), and YOUNG RANGES (the
    // plain uplift path). Erosion then treats each accordingly.
    f32 regimeWavelength { 875.0f };
    f32 hillChainWavelength { 6000.0f }; // ridged hills' own rhythm
    f32 hillChainAmplitude { 130.0f };    // m of hill relief in chains
    f32 oldMassifHeight { 210.0f };       // m of plateau under old hills
    f32 oldMassifHillAmplitude { 150.0f };
    // The LONG swell: a very-slow positive lift of whole landscapes —
    // ranges riding it become truly high peaks, hill country on it
    // becomes highland plateaus. Inland-gated like the massif plateau.
    f32 swellWavelength { 22500.0f };
    // Sized so summits riding the full swell reach ~1200 m — above the
    // cloud layer (800-1000 m) and the snow line, without steepening
    // anything: a base lift carries the peaks, slopes stay local.
    f32 swellHeight { 750.0f };
    // Passability corridors: a mid-frequency band field that locally
    // SOFTENS erosion (never the heights) — cols through ranges,
    // gentle passages between hills. ~quarter of the land.
    f32 gentleWavelength { 1375.0f };
    // Calm-socle breakup: the band field that keeps SOME plains rugged
    // so the habitable family never reads as one uniform carpet.
    f32 calmWavelength { 2600.0f };
    // Objective layers — jittered landmark grids (fbm cannot promise
    // spacing; the grid bounds the distance to the nearest landmark).
    // ALPINE: a 600-900 m summit reachable from anywhere (~6 km),
    // silhouette variants per hash (cone / elongated ridge / rimmed
    // mesa). INTIMATE: a marked hill or an open clearing at ~3 km.
    f32 peakCellSize { 7000.0f };
    f32 peakHeightMin { 600.0f };
    f32 peakHeightMax { 900.0f };
    f32 peakRadiusMin { 1200.0f };
    f32 peakRadiusMax { 2000.0f };
    f32 hillCellSize { 3500.0f };
    f32 hillHeightMin { 120.0f };
    f32 hillHeightMax { 250.0f };
    f32 hillRadiusMin { 600.0f };
    f32 hillRadiusMax { 1200.0f };
    // Valley orientation field: per ~cell angle, smoothly interpolated
    // — the local axis relief stretches along (sightlines) and the
    // trunk valleys run along.
    f32 valleyAxisWavelength { 9000.0f };
    // Master (trunk) valleys: wide flat-floored depressions every
    // ~spacing transverse to nothing in particular — the future fleuve
    // corridors. Their floor joins calm/gentle; crossing a range they
    // read as gorges (kept steep by the uplift, softened by gentle).
    f32 trunkSpacing { 12000.0f };
    f32 trunkDepthMin { 40.0f };
    f32 trunkDepthMax { 80.0f };
    f32 trunkFloorHalfWidth { 600.0f };
    f32 trunkShoulder { 1500.0f };
    // Guaranteed cols: transverse gentle corridors combed along the
    // axis every ~spacing so no range is a regional wall.
    f32 colSpacing { 4000.0f };
    // Lithology: "countries" of rock character between the regime and
    // the carriers — hard pockets keep sharp relief and sea cliffs,
    // soft pockets roll, with no per-case exceptions.
    f32 hardnessWavelength { 4000.0f };
};

class ProceduralControls final : public ControlSource {
public:
    explicit ProceduralControls(const ProceduralControlParams& params)
        : p { params } {}
    ControlSample at(f32 x, f32 z) const override;
    f32 continentalness(f32 x, f32 z) const; // [0,1], warped

    const ProceduralControlParams& params() const { return p; }

private:
    ProceduralControlParams p;
};

// One elevation tier: the macro "floors" the artist/controls pick from —
// plains, hills, plateau, highlands by default. Hills can rise from the
// ground OR from a plateau because the relief rides ON the tier altitude.
struct TierLevel {
    f32 altitude { 0.0f };        // meters
    f32 reliefAmplitude { 0.0f }; // +/- meters of macro relief
    f32 reliefWavelength { 500.0f };
    f32 terrace { 0.0f }; // 0..1 strata quantization strength (mesas)
};

// Real-mountain scale: the highest tier + its relief + the long swell +
// the fastscape orogeny (upliftRate x iterations, minus erosion) put the
// tallest summits at ~1200 m — above the cloud layer (800-1000 m); the
// valleys the stream power carves into that stay walkable.
struct MacroParams {
    vector<TierLevel> tiers {
        { 40.0f, 18.0f, 2100.0f, 0.0f },   // coastal plains
        { 110.0f, 90.0f, 6500.0f, 0.0f },  // hills (long rolling waves)
        { 270.0f, 18.0f, 3500.0f, 0.8f },  // mesa plateau
        { 520.0f, 140.0f, 5500.0f, 0.0f }, // high ranges
    };
    f32 seaLevel { kDefaultSeaLevel };
    f32 seaFloor { -30.0f };   // deep-water altitude (absolute meters)
    f32 shallowDepth { 6.0f }; // below seaLevel over the first shelf band
    f32 shelfWidth { 90.0f };  // meters of shallow shelf past the shore
    f32 seaFalloff { 750.0f }; // meters from shore to the deep floor
    f32 shoreWidth { 220.0f }; // beach ramp band on land
    f32 shoreHeight { 0.8f };  // meters above sea level at the waterline
    // Tiers above this keep their altitude to the rim: the coast becomes
    // a sea cliff instead of a beach ramp.
    f32 cliffTierStart { 1.6f };
    f32 cliffTierEnd { 2.4f };
    // Ridged hill chains of the HILL regime (0 disables — the tests
    // validate a chain-free macro). The control seam copies its own
    // value in at the bake (TileBake); part of MacroParams so
    // synthesizeMacro's input set is ONE struct, not struct + stray arg.
    f32 hillChainWavelength { 0.0f };
    // Anisotropy of the relief carriers along ControlSample's valley
    // axis (1 = isotropic; applied at the sample's axisStrength).
    f32 valleyStretch { 2.5f };
    f32 terraceStep { 40.0f };     // meters between mesa strata
    f32 terraceEdge { 0.16f };     // fraction of a step kept as soft slope
    f32 warpWavelength { 3500.0f }; // relief domain warp
    f32 warpStrength { 700.0f };
    // Elevation recurve: monotone remap of the LAND height above sea
    // level, applied before the coast profile — and before erosion, so
    // S2 re-equilibrates the remapped altitude budget instead of
    // stretching slopes. The three values are the curve outputs at
    // normalized inputs 1/4, 1/2, 3/4 over [0, recurveSpan] meters;
    // 0.25/0.5/0.75 = identity (bit-exact fast path). Push the mid down
    // for flat plains with steep steps, up for domed hills.
    f32 recurveLow { 0.25f };
    f32 recurveMid { 0.5f };
    f32 recurveHigh { 0.75f };
    f32 recurveSpan { 700.0f };
};

struct MacroResult {
    GridSpec spec;
    vector<f32> height;
    vector<f32> uplift;  // [0,1] per texel, stage S2 input
    vector<f32> seaDist; // signed meters to the sea mask (+ on land)
    vector<u8> biome;
    vector<f32> gentle;  // [0,1] passability corridors (erosion softener)
    vector<f32> calm;    // [0,1] calm-socle family (ControlSample::calm)
    vector<f32> trunk;   // [0,1] master-valley floorness (fleuve beds)
    // Regime + swell base lift (m): what the erosion `keep` protects so
    // swelled highlands and old-massif plateaus survive the stream power
    // instead of being carved back toward sea base level.
    vector<f32> plateau;
    // Ridged hill-chain relief amplitude (m): gates the soft-lowland
    // erodibility OFF in hill country so hills keep their equilibrium
    // relief while true plains erode gentle.
    vector<f32> hillRelief;
    // Lithology [0,1] (ControlSample::hardness): erosion character.
    vector<f32> hardness;
};

// Erosion keep from the base lift (TileBake stage 1 and the analytic
// silhouette must agree): fraction of the input height the fluvial pass
// re-blends back, so swelled highlands survive the stream power.
constexpr f32 kPlateauKeepCoef = 0.0008f; // per meter of base lift
constexpr f32 kPlateauKeepMax = 0.5f;
// High calm socles (plateau tops, elevated plains) also resist the
// carve: keep fraction added per unit of altitude-gated calm, so the
// habitable high ground stays high instead of being dissected back to
// base level. Shared by TileBake stage 1 and the analytic mirror.
constexpr f32 kCalmKeep = 0.25f;

// The MacroParams elevation recurve applied to one land height (meters):
// monotone PCHIP through (0,0), (1/4, low), (1/2, mid), (3/4, high),
// (1,1) over [seaLevel, seaLevel + recurveSpan]; identity outside that
// band and at the default control points.
f32 recurveLand(const MacroParams& params, f32 h);

// S1: control fields -> macro elevation. Tier-blended base + domain-warped
// relief + soft terracing + regime extras (plateau, ridged hill chains
// at `hillChainWavelength`; 0 disables them) + coast shelf (beach ramp
// or cliff rim from the tier). Deterministic for its full input set.
MacroResult synthesizeMacro(const ControlSource& controls,
                            const GridSpec& spec, const MacroParams& params,
                            u32 seed);

// Pointwise approximation of the S1 surface (shore falloff derived from
// continentalness instead of the grid distance field): far silhouettes
// beyond baked tiles and the bake's boundary condition.
f32 macroHeightAnalytic(const ProceduralControls& controls,
                        const MacroParams& params, f32 x, f32 z);

} // namespace render::terraingen
