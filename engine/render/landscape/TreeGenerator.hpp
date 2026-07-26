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
    // Wood look. `tubeSides` = ring vertices at full detail (LODs
    // derive: low twin = sides-1, ultra = 3). `curvePreserve` relaxes
    // the chain decimation so the growth trajectory's real bends
    // survive instead of collapsing into straight runs (0 = the fully
    // decimated look). `curveSubdiv` inserts Catmull-Rom points per
    // kept segment, rounding the elbows (halved on the low twin, off
    // on ultra). Defaults reproduce the pre-knob output exactly.
    i32 tubeSides { 5 };          // MAX ring vertices (trunk), 3..12
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
    f32 sideMinFraction { 1.0f }; // 0.25..1
    // Foliage SDF (metaballs at branch tips, order-weighted).
    f32 tipBallRadius { 0.95f };  // order-0 metaball radius (m)
    f32 tipOrderFalloff { 0.78f };// radius x falloff^branchOrder
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

// The leaf-cluster cutout mask every foliage card samples (ONE texture,
// shared — card color stays per-clump vertex color). RGBA8, size x size:
// r = per-leaf brightness (remapped in tree.frag), a = coverage. Pointed
// ellipse leaves at random rotation/size/shade, scattered radially so none
// crosses the card edge (the rectangle silhouette must not survive).
// Deterministic per (seed, params).
vector<u8> generateLeafMaskPixels(u32 size, u32 seed,
                                  const ColonizedTreeParams& params = {});

// Squashed craggy boulder, meant to be sunk slightly into the ground.
MeshData generateRock(u32 seed);

// One or two low foliage blobs hugging the ground.
MeshData generateBush(u32 seed);

} // namespace render
