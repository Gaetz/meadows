#pragma once

#include "engine/core/Defines.hpp"

namespace render {

// The terrain splat array: layer indices match terrain.frag.
enum SplatLayer : u32 {
    SplatLayer_Grass = 0,
    SplatLayer_Rock = 1,
    SplatLayer_Snow = 2,
    SplatLayer_Sand = 3,
    SplatLayer_Count = 4,
};

constexpr u32 kSplatTileSize = 256; // texels per side, per layer

// Deterministic, seamlessly tileable RGBA8 pixels for all layers,
// contiguous (layer-major) — the exact shape Device::createTexture expects
// for an array texture. Procedural (periodic value noise): stylized flat-ish
// albedo, low contrast, in the BotW spirit; swappable for CC0 tiles later.
// Authored in display space, stored as SRGBA8 (linear on sample).
vector<u8> buildSplatTilePixels();

// The grass tile's GREEN channel as the SHADER samples it (SRGB-decoded
// to linear), at tiled uv in [0,1) — the CPU mirror of terrain.frag's
// `wander` term, so sim consumers (footstep material) agree with the
// VISIBLE snow/sand borders instead of tracing the raw altitude contour.
f32 splatWander(f32 u, f32 v);

// The grass ground color at tiled uv in [0,1), in DISPLAY space —
// currently fully uniform. Grass blades inherit it at their root so
// the carpet and the ground share ONE color source (grass.vert
// decodes to linear like the terrain's sRGB sampler).
Vec3 grassAlbedo(f32 u, f32 v);

} // namespace render
