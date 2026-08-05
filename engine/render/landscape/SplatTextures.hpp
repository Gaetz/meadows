#pragma once

#include "engine/core/Defines.hpp"

namespace render {

// The terrain splat array: layer indices match terrain.frag.
enum SplatLayer : u32 {
    SplatLayer_Grass = 0,
    SplatLayer_Rock = 1,
    SplatLayer_Snow = 2,
    SplatLayer_Sand = 3,
    SplatLayer_Cliff = 4,
    SplatLayer_Count = 5,
};

constexpr u32 kSplatTileSize = 256; // texels per side, per layer

// Grass-family ground variants (docs/GRASS-REDO.md): the semantic grass
// layer has 4 texture variants zoned by terrain_zones.glsl / grassZoneAt.
// Variant 0 IS the base grass layer; variants 1-3 live as extra array
// layers after the semantic five. The semantic weights never change —
// variants only redirect the grass FETCH.
constexpr u32 kGrassVariantCount = 4;
// Family variants (hex-tiling per family — terrain_zones.glsl
// hexFamilyLayer mirrors this table): rock 4-way (base + 8/9/10),
// snow 3-way (base + 11/12), sand 4-way (base + 13/14/15). Cliff keeps
// its single strata-modulated layer.
constexpr u32 kRockVariantCount = 4;
constexpr u32 kSnowVariantCount = 3;
constexpr u32 kSandVariantCount = 4;
// Scree (talus at rock feet — terrain_weights.glsl screeFactor): a
// dedicated sand-family layer the hex pick flips to under the scree
// bias, so it blends with sand at its fringe and with grass through
// the ordinary weight falloff.
constexpr u32 kScreeLayer = 16;
constexpr u32 kSplatArrayLayers = 17;
constexpr u32 grassVariantLayer(u32 variant) {
    return variant == 0 ? SplatLayer_Grass : SplatLayer_Count + variant - 1;
}
constexpr u32 rockVariantLayer(u32 variant) {
    return variant == 0 ? SplatLayer_Rock : 7 + variant;
}
constexpr u32 snowVariantLayer(u32 variant) {
    return variant == 0 ? SplatLayer_Snow : 10 + variant;
}
constexpr u32 sandVariantLayer(u32 variant) {
    return variant == 0 ? SplatLayer_Sand : 12 + variant;
}

// Deterministic, seamlessly tileable RGBA8 pixels for all layers,
// contiguous (layer-major) — the exact shape Device::createTexture expects
// for an array texture. Procedural (periodic value noise): stylized flat-ish
// albedo, low contrast, in the BotW spirit; swappable for CC0 tiles later.
// Authored in display space, stored as SRGBA8 (linear on sample).
vector<u8> buildSplatTilePixels();

// Per-layer displacement heights in [0,1] for the height-based layer blend
// (terrain_blend.glsl), correlated with each tile's albedo structure (same
// noise seeds). f32 per texel, layer-major — the R16F initial-data shape.
// The cooked .mtex height array replaces this when resident.
vector<f32> buildSplatHeightPixels();

// Per-layer tangent-space normals derived from the SAME height functions
// (central differences, wrapped): rg = xy * 0.5 + 0.5, z reconstructed
// in-shader — the BC5 convention the cooked arrays use, so terrain.frag
// has ONE decode path. RGBA8 layer-major.
vector<u8> buildSplatNormalPixels();

// The grass tile's ±1% luminance drift alone (mean 1) — the blade root
// bake multiplies it over the ACTIVE material set's per-variant mean
// color (TerrainSystem::grassAlbedoBase), so the meadow keeps its life on
// the cooked set too.
f32 grassBlotch(f32 u, f32 v);

// The grass ground color at tiled uv in [0,1), in DISPLAY space —
// near-uniform (±1% luminance drift). Grass blades inherit it at their
// root so the carpet and the ground share ONE color source (grass.vert
// decodes to linear like the terrain's sRGB sampler).
Vec3 grassAlbedo(f32 u, f32 v);

} // namespace render
