#pragma once

#include <filesystem>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

// Authored-terrain plumbing (chantier 2 B8): TerrainPatchForm records +
// `.ter` delta-grid assets -> the engine's immutable render::HeightPatches
// overlay (which rides inside TerrainParams — every height consumer is
// patched without a signature change). The sculpt tool edits grids in
// memory, PUBLISHES a fresh overlay (immutability keeps workers race-free)
// and saves through writeTerFile + TerrainPatchForm records (EditSession).

namespace world {

// .ter file: magic "TER1", u32 sample count n, then n*n f32 deltas
// (row-major, x fastest, rows along +Z), little-endian.
bool writeTerFile(const std::filesystem::path& path,
                  const render::HeightPatch& patch);
std::optional<render::HeightPatch> readTerFile(
    const std::filesystem::path& path);

// Builds the overlay from every resolved TerrainPatchForm; missing or
// unreadable assets are skipped (logged). An empty database yields an
// overlay with no chunks — height() then matches the pure noise.
sptr<const render::HeightPatches> buildHeightPatches(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets,
    f32 chunkSize = 64.0f);

} // namespace world
