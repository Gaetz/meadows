#pragma once

// cook-terrain-materials — manifest of source PNG maps -> the four .mtex
// texture arrays the terrain material system samples (albedo BC7_SRGB,
// normal BC5, ORM BC7_UNORM, height R16_UNORM), 512² with offline mips.
// Pipeline and layout contract: docs/AUDIT/TERRAIN-TEXTURING.md §3-4 and
// engine/assets/CookedTexture.hpp.
namespace cooker {

int cookTerrainMaterials(const char* manifestPath, const char* outDir);

} // namespace cooker
