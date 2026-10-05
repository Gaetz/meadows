#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp" // render::kDefaultSeaLevel
#include "engine/terrain/generation/WorldLayer.hpp"

// Terrain generation pipeline — stage S1 (macro synthesis). Headless
// (lib meadows): the bake runs on JobSystem workers or in doctests, never
// on the render side. docs/PAYSAGE.md §1.3 holds the pipeline overview.
//
// The pipeline consumes CONTROL FIELDS (elevation tier, uplift, sea,
// biome, calm, gentle, trunk...) through the ControlSource seam. The one
// production provider is ProceduralControls (everything derived from
// the world seed); the seam exists so painted/authored control maps can
// feed the same stages later (docs/PAYSAGE.md §6.1) — nothing downstream
// knows which provider fed it.

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
    // The elevation FLOOR in meters ABOVE SEA LEVEL when the provider
    // knows it (the world layer's étage): landHeight builds on it
    // instead of the tier table's altitude — the table then only
    // shapes the relief. hasBase = false (painted/test sources) = the
    // legacy absolute tier floor.
    f32 base { 0.0f };
    bool hasBase { false };
    // Valley-bed depth (m) the macro digs along a ridged bed field
    // (the pre-erosion drainage skeleton of the province); 0 = none.
    f32 bedDepth { 0.0f };
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
    // Biome id alone, with the (heavy) elevation tier already known: the
    // macro synthesis samples the control fields on a coarse lattice and
    // interpolates, but an id cannot interpolate — nearest-sampling it
    // there drew 64 m axis-aligned biome stairs. Providers whose biome
    // derives from cheap fields override this so the synthesis can ask
    // PER TEXEL; the default pays one full sample (painted/test sources
    // are lookup-cheap anyway).
    virtual u8 biomeIdAt(f32 x, f32 z, f32 tier) const {
        (void)tier;
        return at(x, z).biome;
    }
};

// The local RHYTHM drawn on the world layer's floor (docs/PAYSAGE.md
// §7.5): pieces (one landmark per cell — a dome, a ridge with cols, a
// mesa), the massif crests, the valley beds, the guaranteed cols and
// the lithology. Everything indexed by étage reads the province the
// piece or point stands in, so the same rhythm scales with the floor.
struct RhythmParams {
    f32 pieceCellSize { 7000.0f };
    f32 pieceChance { 0.8f };
    f32 pieceRadiusMin { 900.0f };
    f32 pieceRadiusMax { 1800.0f };
    // [étage][min, max] meters of lift at the piece's summit.
    f32 pieceHeightByEtage[4][2] { { 120.0f, 220.0f },
                                   { 180.0f, 320.0f },
                                   { 120.0f, 260.0f },
                                   { 350.0f, 650.0f } };
    f32 ridgeColWavelength { 1300.0f }; // saddles along a ridge piece
    f32 crestWavelength { 1800.0f };    // massif ridged crests
    f32 crestAmplitudeByEtage[4] { 30.0f, 60.0f, 90.0f, 200.0f };
    f32 bedWavelength { 3000.0f };      // valley-bed skeleton
    f32 bedDepthByEtage[4] { 14.0f, 22.0f, 20.0f, 35.0f };
    f32 colSpacing { 2500.0f };         // guaranteed passes in massifs
    f32 hardnessWavelength { 4000.0f };
};

// Sandbox controls: every field derives from ONE sample of the world
// layer (sea, floor, étage, massif, coast, climate) plus the local
// rhythm. Pure functions of (seed, x, z) — infinite, deterministic,
// and continuous across map lines (no per-map state anywhere).
struct ProceduralControlParams {
    u32 seed { 1337 }; // copied into world.seed by ProceduralControls
    WorldLayerParams world;
    RhythmParams rhythm;
};

class ProceduralControls final : public ControlSource {
public:
    explicit ProceduralControls(const ProceduralControlParams& params)
        : p { params } {
        p.world.seed = p.seed;
    }
    ControlSample at(f32 x, f32 z) const override;
    // Same sample, handing back the world sample it derived from —
    // macroHeightAnalytic needs the continent value again for its
    // shore distance. One evaluation, bit-identical to calling both.
    ControlSample at(f32 x, f32 z, WorldSample& outWorld) const;
    u8 biomeIdAt(f32 x, f32 z, f32 tier) const override; // climate only

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

// The étage table (docs/PAYSAGE.md §7.5): one tier per elevation
// province of the world layer (WorldLayerParams::etageAltitude indexes
// it through ControlSample::tier). With a world floor the altitude
// column is unused — it is the ABSOLUTE floor of painted/test sources
// only; the relief columns are the province's own walking rhythm:
// plains roll +/-25 m over 700 m, hills +/-45 m, the plateau is flat
// and terraced, the high mountain carries the big waves the massif
// crests and the erosion sculpt.
struct MacroParams {
    vector<TierLevel> tiers {
        { 40.0f, 25.0f, 700.0f, 0.0f },     // plains: gentle hills
        { 150.0f, 45.0f, 900.0f, 0.0f },    // hills
        { 450.0f, 28.0f, 750.0f, 0.5f },    // plateau (terraced)
        { 1200.0f, 120.0f, 1100.0f, 0.0f }, // high mountain
    };
    f32 seaLevel { kDefaultSeaLevel };
    // Two-stage ocean: shore ramp -> luminous COASTAL PLATEAU (the
    // bright turquoise band the player reads as swimmable) -> talus ->
    // dark open-sea floor. Depths are calibrated to the water RENDER's
    // absorption (the old -30 m floor already read as abyss), not to
    // real bathymetry — the contrast comes from the shallow plateau.
    f32 seaFloor { -70.0f };    // deep-water altitude (absolute meters)
    f32 shallowDepth { 6.0f };  // below seaLevel at the shelf's inner edge
    f32 shelfWidth { 90.0f };   // meters of nearshore ramp past the shore
    f32 shelfDepth { 14.0f };   // below seaLevel across the plateau
    f32 shelfEnd { 600.0f };    // meters from shore where the plateau ends
    f32 seaFalloff { 2500.0f }; // meters from shore to the deep floor
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
    // Valley-bed skeleton wavelength for ControlSample::bedDepth (the
    // control seam copies its own value in at the bake, like
    // hillChainWavelength). 0 disables.
    f32 bedWavelength { 0.0f };
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

// Bounded-map BORDER transitions (chantier CARTES v2,
// docs/PAYSAGE.md §1.1 — design dev): a transition belongs to the
// BORDER LINE, not to a map. Each border hashes (seed, line identity)
// to Sea or Ridges — symmetric by construction, both neighbours agree.
// The shape is a pure function of the distance to the line, applied
// identically by both maps' bakes AND the runtime fallback:
//   Ridges — a range rising PROGRESSIVELY on both sides
//     (kMapBorderMountainHalf each side, crest ON the line), its
//     height varying along the line (low-frequency noise -> natural
//     peaks and SADDLES: the cols emerge from the system);
//   Sea — a genuine sea arm (both coasts descend over
//     kMapBorderSeaHalf), with occasional ISLETS mid-channel where the
//     along-line noise says so (land appears progressively).
// Corners compose by construction: two mountain lines join (max of
// profiles); a mountain line dives into a sea line as coastal cliffs
// (the sea cut applies after the lift); sea+sea is open ocean.
enum class MapEdgeStyle : i32 { Sea = 0, Ridges = 1 };

// The hashed style PROPOSAL of one border LINE segment: `lineIndex` is
// the grid index of the line (x = lineIndex * mapSize for vertical),
// `cellCross` the map coordinate along the crossing axis.
MapEdgeStyle mapBorderStyle(u32 seed, i32 lineIndex, i32 cellCross,
                            bool vertical);


struct MapGridSpec {
    bool valid { false };
    u32 seed { 0 };
    f32 mapSize { 24576.0f };
    f32 seaLevel { kDefaultSeaLevel };
};

constexpr f32 kMapBorderMountainHalf = 900.0f; // rise, each side
constexpr f32 kMapBorderMountainLift = 260.0f;  // max crest above base
constexpr f32 kMapBorderSeaHalf = 900.0f;       // coast, each side
constexpr f32 kMapBorderSeaDepth = 40.0f;       // channel floor
constexpr f32 kMapBorderCrestWavelength = 2000.0f; // peaks/saddles
constexpr f32 kMapBorderRidgeKeep = 0.8f; // stage-1 erosion keep
// The line itself MEANDERS: its position is warped by a long-range
// wave along the line (the terrain's own domain-warp idea), so coasts
// and ranges wander instead of ruling straight. Both maps and the
// fallback share the same pure warp — symmetry survives.
constexpr f32 kMapBorderWander = 300.0f;            // max offset
constexpr f32 kMapBorderWanderWavelength = 3000.0f; // long-range wave
// Coherence with the UNDERLYING terrain (the proximity rule): the
// transition reads the ground it stands on, via the input height. A
// range only rises from LAND (the gate fades across this band around
// sea level), a sea arm only DEEPENS (never lifts an open-ocean floor
// into a shelf), and islets are drowned land — never mid-ocean chains.
constexpr f32 kMapBorderLandFadeLow = -8.0f;  // vs seaLevel
constexpr f32 kMapBorderLandFadeHigh = 24.0f; // full strength above
// Styles are hashed per line SEGMENT; around a segment junction the
// two styles cross-fade over this band along the line — a sea arm
// closes into a bay while the range rises, never a dead-end channel.
constexpr f32 kMapBorderStyleBlend = 900.0f;
// A Sea proposal needs at least this fraction of its segment's samples
// under sea in the ANALYTIC world to stand; otherwise it demotes to
// Ridges (no 4 km canal dug across a continent).
constexpr f32 kMapBorderSeaVetoOceanFrac = 0.34f;

// The RESOLVED style of a segment: the hashed proposal, with the Sea
// veto above applied against the analytic ground sampled along the
// nominal line (memoized per segment). Every caller passes the same
// (controls, macro) it feeds macroHeightAnalytic, so the bakes and
// the runtime fallback resolve identically.
MapEdgeStyle mapBorderStyleResolved(const ProceduralControls& controls,
                                    const MacroParams& macro,
                                    const MapGridSpec& spec,
                                    i32 lineIndex, i32 cellCross,
                                    bool vertical);

f32 applyMapGridShape(const ProceduralControls& controls,
                      const MacroParams& macro, const MapGridSpec& spec,
                      f32 x, f32 z, f32 h);

// The mountain factor alone (0 away, 1 on a ridge line): the stage-1
// erosion KEEP for the ranges — without protection the artificial
// crest has no plateau field and the fastscape carves it back down.
// `h` is the (shaped) terrain height there: the keep carries the same
// land gate as the lift, so ocean stretches keep no phantom crest.
f32 mapGridRidgeFactor(const ProceduralControls& controls,
                       const MacroParams& macro, const MapGridSpec& spec,
                       f32 x, f32 z, f32 h);

// Pointwise mirror of the S1 surface (shore falloff derived from the
// world layer's continent value instead of the grid distance field):
// far silhouettes beyond baked tiles, the bake's boundary condition,
// the master network's grid. Equal to the per-texel macro away from
// the coast (no erosion compression: the bake's hard erosion budget
// keeps the baked ground near this surface — docs/PAYSAGE.md §7.5).
f32 macroHeightAnalytic(const ProceduralControls& controls,
                        const MacroParams& params, f32 x, f32 z);

} // namespace render::terraingen
