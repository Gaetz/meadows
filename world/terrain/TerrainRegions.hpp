#pragma once

#include <filesystem>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/terrain/TerrainBase.hpp"

// Baked-terrain region IO: `.trg` assets carry the absolute height grid and
// the coarse mask channels of one render::TerrainRegion. Detail-noise knobs
// (amplitude/wavelength/octaves) are NOT in the asset — they live on
// TerrainRegionForm so they stay field-level moddable; the builder applies
// them after reading. Sibling of TerrainPatches (.ter deltas), which stays
// untouched: absolute base and authored deltas are different layers.

namespace world {

// .trg file: magic "TRG2", u32 width, u32 height, f32 originX, f32 originZ,
// f32 texelSize, f32 edgeBlend, width*height f32 heights (row-major, x
// fastest, rows along +Z), u32 maskWidth, u32 maskHeight (0 0 = no masks),
// then the six u8 channels (detailAmp, flow, wetness, beach, biome,
// rockExposure) of maskWidth*maskHeight each. Little-endian.
bool writeTrgFile(const std::filesystem::path& path,
                  const render::TerrainRegion& region);
std::optional<render::TerrainRegion> readTrgFile(
    const std::filesystem::path& path);

// Builds the baked base from every resolved TerrainRegionForm; missing or
// unreadable assets are skipped (logged). An empty database yields a base
// with no regions — height() then matches the pure noise.
sptr<const render::TerrainBase> buildTerrainBase(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets);

} // namespace world
