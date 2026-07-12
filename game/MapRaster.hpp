#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

// Chantier 9 C9.6 — the in-game map raster: a stylized top-down RGBA image
// of an exterior worldspace, generated on the CPU from the SAME pure
// terrain functions the renderer and collision use (render::terrain::
// height / materialWeights — deterministic, authored .ter patches
// included via TerrainParams.patches). No GL, no rhi: bytes in, bytes out
// — headless-testable (MapRasterTest), pushed to the UI through
// UiSystem::setRuntimeTexture.

namespace game {

// The raster request: world extent [minX..maxX] x [minZ..maxZ] rendered
// into a size x size RGBA8 image. Row 0 = minZ (world +Z runs DOWN the
// image), column 0 = minX — the same mapping mapUv() below exposes, so
// markers positioned by CSS left/top percentages land on their pixel.
struct MapRasterDesc {
    const render::TerrainParams* terrain { nullptr };
    f32 minX { 0.0f };
    f32 minZ { 0.0f };
    f32 maxX { 1.0f };
    f32 maxZ { 1.0f };
    u32 size { 512 };
};

// World position -> map UV in [0, 1] (clamped). u = left->right (+X),
// v = top->bottom (+Z) — THE one world->map mapping; the raster loop and
// every marker/POI placement go through it so they can never disagree.
Vec2 mapUv(const MapRasterDesc& desc, f32 worldX, f32 worldZ);

// Generates the stylized raster: water below seaLevel (depth-blended
// blues), land tinted by the material weights (grass/rock/snow/sand) and
// shaded by the terrain normal + a quantized hypsometric lift. Returns
// size * size * 4 tightly packed RGBA bytes (alpha 255). Deterministic:
// the same desc gives bit-identical bytes.
vector<u8> generateMapRaster(const MapRasterDesc& desc);

} // namespace game
