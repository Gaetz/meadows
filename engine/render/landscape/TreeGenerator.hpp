#pragma once

#include "engine/core/Defines.hpp"
#include "engine/assets/MeshData.hpp"

namespace render {

// Procedural low-poly props, deterministic per seed (same seed =
// bit-identical mesh — doctested), flat-shaded, vertex-colored in linear
// space. uv.x carries the wind sway weight (0 = rigid, 1 = outer foliage),
// uv.y the normalized height — tree.vert uses both.

// Tree-builder params: every artistic knob of each generator
// is a FLAT engine struct — defaults reproduce the shipped look; the
// moddable source of truth is the matching *TreeTuningForm (data/), mapped
// here by the scene (the TerrainParams pattern — engine never sees Forms).

// A tree is ONE opaque mesh (full stylized canopy — the BotW look;
// cutout leaf cards lost the fill-rate A/B, docs/RENDERING.md).
// Tall trunk column, 3-5 short upward branches near the top, one rounded
// foliage lobe per branch tip plus a crown. The canopy's normals are
// SPHERIZED on the mesh: each vertex blends its lobe-center direction with
// the whole-canopy direction, so light pools per lobe while staying
// coherent across the tree — the halisavakis/BotW trick applied to solid
// geometry. Early-Z friendly, and the same mesh casts the shadows.
struct LobeTreeParams {
    f32 trunkHeightMin { 4.2f };  // meters (before instance scale)
    f32 trunkHeightMax { 6.1f };
    f32 trunkRadiusMin { 0.17f };
    f32 trunkRadiusMax { 0.24f };
    f32 trunkTaper { 0.42f };     // top radius = base radius x taper
    f32 lean { 0.16f };           // trunk lean jitter (xz per unit up)
    i32 branchCountMin { 3 };
    i32 branchCountMax { 5 };
    f32 branchLengthMin { 0.9f };
    f32 branchLengthMax { 1.7f };
    f32 crownLobeRadiusMin { 0.85f }; // the trunk-top lobe
    f32 crownLobeRadiusMax { 1.25f };
    f32 branchLobeRadiusMin { 0.60f }; // one per branch tip
    f32 branchLobeRadiusMax { 0.98f };
    f32 lobeFlatten { 0.85f };    // y squash (crowns, not balls)
    f32 normalSpherize { 0.4f };  // lobe->whole-canopy normal blend
};

// `lobeSubdivisions` is the canopy LOD: 2 = 320-face lobes (near), 1 = 80
// faces (distance, shadow casters, reflections — 4x cheaper on vertices).
// The SAME seed drives both: composition and colors match across LODs,
// only the lobe tessellation changes.
MeshData generateTree(u32 seed, u32 lobeSubdivisions = 2,
                      const LobeTreeParams& params = {});

// The default tree (docs/RENDERING.md brique 27b) —
// skeleton grown by the Runions et al. 2007 space colonization algorithm
// (attraction points in a crown envelope, iterative growth, pipe-model
// radii), foliage as CROSS-PLANE clusters scattered inside an SDF built
// from smooth-merged metaballs at the branch tips (radius weighted by
// branch order so deep twigs don't inflate the volume). The card normals
// are the SDF GRADIENT — the canopy shades as one smooth volume (the
// Genshin/BotW trick, this time on cards instead of solid lobes).
// The colonized tree's knobs (defaults = the shipped look).
struct ColonizedTreeParams {
    // Skeleton (Runions 2007).
    f32 segment { 0.28f };        // D — growth step (m)
    f32 killDistance { 0.70f };   // d_k (m)
    i32 attractorCount { 350 };   // N in the crown envelope
    f32 pipeExponent { 2.6f };    // radii merge (2..3)
    f32 tropism { 0.14f };        // upward growth bias
    // Crown envelope (ellipsoid; ranges rolled per seed).
    f32 trunkBaseMin { 1.6f };    // bare trunk below the crown (m)
    f32 trunkBaseMax { 2.5f };
    f32 crownHeightMin { 2.6f };
    f32 crownHeightMax { 3.8f };
    f32 crownRadiusMin { 1.9f };
    f32 crownRadiusMax { 3.0f };
    // --- Conifer habit (defaults neutral = the broadleaf look, and
    // bit-exact: at 0 none of these consumes a random draw). ---
    // Crown radius profile: 0 = the ellipsoid, 1 = a cone — full radius
    // at the crown base shrinking to the apex (spruce/fir silhouette).
    f32 crownTaper { 0.0f };
    // Fraction of attractors packed in a thin axial column: an apical
    // LEADER the trunk climbs straight through (monopodial habit).
    f32 leaderBias { 0.0f };
    // Presses side growth toward the horizontal with a slight droop —
    // the whorled shelves of a spruce. 0 = free growth.
    f32 lateralFlatten { 0.0f };
    // Foliage cards ride the OUTER BRANCHES (sprays) instead of the SDF
    // shell; the shading normal stays the canopy gradient either way.
    // Blend 0..1 = share of cards that spray.
    f32 sprayFoliage { 0.0f };
    // Leaf-mask ATLAS slot this species' cards sample (0..7; the slot's
    // raster is generated from THIS species' leaf params + shape).
    i32 leafStyle { 0 };
    // Raster shape of the claimed slot: 0 pointed ellipse, 1 needles,
    // 2 rounded, 3 lobed (maple-ish), 4 serrated.
    i32 leafShape { 0 };
    // Seasons (runtime, no mesh rebuild): the shader mixes the card
    // color toward autumnTint by (global season x seasonality), and
    // drops cards by (global leaf-fall x seasonality) — conifers set
    // seasonality 0 and stay green through winter.
    Vec3 autumnTint { 0.62f, 0.30f, 0.08f };
    f32 seasonality { 1.0f };
    // Wood look. `tubeSides` = ring vertices at full detail (LODs
    // derive: low twin = sides-1, ultra = 3). `curvePreserve` relaxes
    // the chain decimation so the growth trajectory's real bends
    // survive instead of collapsing into straight runs (0 = the fully
    // decimated look). `curveSubdiv` inserts Catmull-Rom points per
    // kept segment, rounding the elbows (halved on the low twin, off
    // on ultra). Defaults reproduce the pre-knob output exactly.
    // Root flare: near the ground the trunk widens into buttress
    // lobes (radial multiplier on the root chain's rings — angular
    // noise x height falloff, phases rolled from the tree seed).
    // `flareAmount` = extra radius at ground as a multiple of the
    // trunk radius (0 = off), `flareHeight` = meters it decays over,
    // `flareLobes` = angular bump count.
    f32 flareAmount { 0.6f };
    f32 flareHeight { 1.2f };
    i32 flareLobes { 4 };
    i32 tubeSides { 12 };         // MAX ring vertices (trunk), 3..12
    f32 curvePreserve { 0.0f };   // 0..1
    i32 curveSubdiv { 0 };        // 0..3
    // `pathJitter` kinks the kept trajectory points (deterministic per
    // node — LODs agree); with subdivision the kinks round into waves,
    // without it they stay sharp breaks. `ringIrregularity` makes the
    // tube faces angularly uneven (very different widths at 1).
    // `sideMinFraction` tapers the ring count by HALVING: the trunk
    // uses tubeSides and thinner chains halve their count each time
    // their radius halves, down to tubeSides x fraction (floor 3;
    // 1 = constant count — the pre-knob behavior). Pick an EVEN
    // tubeSides for clean halvings (12 -> 6 -> 3, 8 -> 4).
    f32 pathJitter { 0.0f };      // 0..1
    f32 ringIrregularity { 0.0f };// 0..1
    f32 sideMinFraction { 0.5f }; // 0.25..1
    // Foliage SDF (metaballs at branch tips, order-weighted).
    f32 tipBallRadius { 0.95f };  // order-0 metaball radius (m)
    f32 tipOrderFalloff { 0.78f };// radius x falloff^branchOrder
    // Metaball radius floor. The 0.30 default is the shipped tree look;
    // BUSH species drop it (~0.06) so a knee-high canopy stays tight.
    f32 tipBallMin { 0.30f };
    f32 smoothK { 0.7f };         // metaball smooth-min width (m)
    // Billboard cards.
    f32 cardHalfSizeMin { 0.084f };
    f32 cardHalfSizeMax { 0.144f };
    f32 densityGradient { 3.0f }; // xG top, x1 sides, x1/G underneath
    f32 foliageDensity { 2.5f };  // card-count multiplier (per-LOD bases)
    // Leaf mask (the shared cutout texture every card samples).
    i32 leafCount { 60 };         // leaves scattered into the mask
    f32 leafSizeMin { 0.10f };    // leaf length, fraction of the card
    f32 leafSizeMax { 0.25f };
    // Cutout -> solid ramp against the sampled MIP (log2 of the card's
    // on-screen footprint, so distance and tree scale are both in):
    // below start = crisp leaves, past end = full card (the distant
    // canopy mass). Rendered live via uLeafLodInfo, not baked.
    f32 leafSolidStart { 4.0f };
    f32 leafSolidEnd { 7.0f };
    // Bark material (draw-time — pushed per variant draw, no mesh
    // rebuild): triplanar tile density, hex-tiling lattice cell + seam
    // sharpness, and a tint multiplier over the bark texture
    // (1,1,1 = the texture's own hue).
    f32 barkTileScale { 0.3f };    // tiles per meter
    f32 barkHexCell { 0.85f };     // hex lattice cell (uv units)
    f32 barkHexSharpness { 6.0f }; // barycentric exponent
    Vec3 barkTint { 1.0f, 1.0f, 1.0f };
};

// `detail` mirrors the LOD contract: 2 near, 1 mid, 0 far (tube sides
// and cluster count scale; skeleton and silhouette are seed-stable
// across levels — params feed BOTH, so all LODs must share one params
// set). Deterministic per (seed, params), worker-safe.
MeshData generateColonizedTree(u32 seed, u32 detail = 2,
                               const ColonizedTreeParams& params = {});

// Shadow-caster proxy for the far cascades: the same seed-stable skeleton
// with coarse wood, but the canopy as SOLID 20-face icosahedra on the SDF
// metaballs instead of the billboard-card cloud — ~6x fewer vertices, no
// alpha test, and a stable full-mass shadow (cards re-aim at the light
// every frame; blobs don't). Cascade 0 keeps the leafy cutout casters.
MeshData generateColonizedTreeShadowProxy(u32 seed,
                                          const ColonizedTreeParams& params
                                          = {});

// The leaf-cluster cutout mask every foliage card samples (an ATLAS —
// left half broadleaf, right half needles; the card's flag bias picks
// the slot). RGBA8, one size x size tile: r = per-leaf brightness
// (remapped in tree.frag), a = coverage. Leaves at random rotation/
// size/shade, scattered radially so none crosses the card edge; `shape`
// picks the leaf outline (see ColonizedTreeParams::leafShape).
// Deterministic per (seed, params, shape).
vector<u8> generateLeafMaskPixels(u32 size, u32 seed,
                                  const ColonizedTreeParams& params = {},
                                  i32 shape = 0);

// Squashed craggy boulder, meant to be sunk slightly into the ground.
MeshData generateRock(u32 seed);

// One or two low foliage blobs hugging the ground.
MeshData generateBush(u32 seed);

} // namespace render
