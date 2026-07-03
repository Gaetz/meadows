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
//
// Authored in display space and stored as RGBA8 for now; switches to SRGBA8
// once the HDR/tonemap brick handles gamma on output.
vector<u8> buildSplatTilePixels();

} // namespace render
