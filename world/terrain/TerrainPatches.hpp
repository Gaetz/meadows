#pragma once

#include <filesystem>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "data/plugins/Record.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/terrain/HeightPatches.hpp"
#include "world/worldspace/WorldForms.hpp"

// Authored-terrain plumbing: TerrainPatchForm records +
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

// The save's terrain (chantier ESPRITS E2.b — the first save that carries
// ASSETS): writes one .ter per listed chunk of `patches` into `dir`
// (patch_<cx>_<cz>.ter) and stages the plugin records + asset entries —
// a PATCH of the base TerrainPatchForm's `asset` when the chunk is already
// authored, a `creates` record under a deterministic guid otherwise. The
// asset guid is per chunk in the save namespace (kSaveTerrainAssetPrefix),
// so a reload re-staged by the next save keeps the same ids. `assetPrefix`
// is the entry path's directory relative to the plugin file.
constexpr const char* kSaveTerrainAssetPrefix = "7e88a112-0000-4000-8000-";
constexpr const char* kSaveTerrainRecordPrefix = "7e88a111-0000-4000-8000-";
core::Guid saveTerrainAssetGuid(u64 chunkKey);
bool isSaveTerrainAsset(const core::Guid& asset);
void stageTerrainPatchRecords(const render::HeightPatches& patches,
                              const vector<u64>& chunks,
                              const data::FormDatabase& forms,
                              const core::Guid& worldspace,
                              const std::filesystem::path& dir,
                              const str& assetPrefix,
                              vector<data::Record>& records,
                              vector<data::AssetEntry>& assets);

// Builds the overlay from every resolved TerrainPatchForm; missing or
// unreadable assets are skipped (logged). An empty database yields an
// overlay with no chunks — height() then matches the pure noise.
sptr<const render::HeightPatches> buildHeightPatches(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets,
    f32 chunkSize = 64.0f, const WorldspaceFilter& filter = {});

} // namespace world
