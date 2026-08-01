#pragma once

#include <filesystem>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/terrain/BiomeMap.hpp"

// BiomeForm records (+ optional BiomeMapForm painted index map) -> the
// engine's immutable render::BiomeSet. Sibling of buildHeightPatches /
// buildTerrainBase / buildWaterBodies.

namespace world {

// .tbm file: magic "TBM1", u32 width, u32 height, f32 originX,
// f32 originZ, f32 texelSize, then width*height u8 biome indices
// (row-major, x fastest, rows along +Z). Little-endian.
struct BiomeIndexMap {
    f32 originX { 0.0f };
    f32 originZ { 0.0f };
    f32 texelSize { 16.0f };
    u32 width { 0 };
    u32 height { 0 };
    vector<u8> indices;
};
bool writeTbmFile(const std::filesystem::path& path,
                  const BiomeIndexMap& map);
std::optional<BiomeIndexMap> readTbmFile(const std::filesystem::path& path);

// Table indexed by BiomeForm::paletteIndex (gaps filled with the neutral
// biome); the painted map rides along when a BiomeMapForm resolves.
sptr<const render::BiomeSet> buildBiomeSet(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets);

} // namespace world
