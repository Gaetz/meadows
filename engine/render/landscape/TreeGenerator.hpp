#pragma once

#include "engine/core/Defines.hpp"
#include "engine/assets/MeshData.hpp"

namespace render {

// Procedural low-poly props, deterministic per seed (same seed =
// bit-identical mesh — doctested), flat-shaded, vertex-colored in linear
// space. uv.x carries the wind sway weight (0 = rigid, 1 = outer foliage),
// uv.y the normalized height — tree.vert uses both.

// Tree-builder params (2026-07-20): every artistic knob of each generator
// is a FLAT engine struct — defaults reproduce the shipped look; the
// moddable source of truth is the matching *TreeTuningForm (data/), mapped
// here by the scene (the TerrainParams pattern — engine never sees Forms).

// A tree is ONE opaque mesh (brick 27: full stylized canopy — the BotW
// look; the leaf-card system lost the fill-rate A/B and was removed).
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

// EXPERIMENT (feature/space-colonization-trees): alternative tree —
// skeleton grown by the Runions et al. 2007 space colonization algorithm
// (attraction points in a crown envelope, iterative growth, pipe-model
// radii), foliage as CROSS-PLANE clusters scattered inside an SDF built
// from smooth-merged metaballs at the branch tips (radius weighted by
// branch order so deep twigs don't inflate the volume). The card normals
// are the SDF GRADIENT — the canopy shades as one smooth volume (the
// Genshin/BotW trick, this time on cards instead of solid lobes).
// The colonized tree's knobs (defaults = the dev-tuned 2026-07-20 look).
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
    // Foliage SDF (metaballs at branch tips, order-weighted).
    f32 tipBallRadius { 0.95f };  // order-0 metaball radius (m)
    f32 tipOrderFalloff { 0.78f };// radius x falloff^branchOrder
    f32 smoothK { 0.7f };         // metaball smooth-min width (m)
    // Billboard cards.
    f32 cardHalfSizeMin { 0.042f };
    f32 cardHalfSizeMax { 0.072f };
    f32 densityGradient { 3.0f }; // xG top, x1 sides, x1/G underneath
    f32 foliageDensity { 3.2f };  // card-count multiplier (per-LOD bases)
};

// `detail` mirrors the LOD contract: 2 near, 1 mid, 0 far (tube sides
// and cluster count scale; skeleton and silhouette are seed-stable
// across levels — params feed BOTH, so all LODs must share one params
// set). Deterministic per (seed, params), worker-safe.
MeshData generateColonizedTree(u32 seed, u32 detail = 2,
                               const ColonizedTreeParams& params = {});

// Squashed craggy boulder, meant to be sunk slightly into the ground.
MeshData generateRock(u32 seed);

// One or two low foliage blobs hugging the ground.
MeshData generateBush(u32 seed);

} // namespace render
