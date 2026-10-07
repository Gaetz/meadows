#pragma once

#include <filesystem>

#include "data/forms/LandscapeForms.hpp"
#include "engine/terrain/generation/TileBake.hpp"

// The generation PUPITRE's plumbing (docs/PAYSAGE.md §7.7, brick Z0):
// TerrainGenTuningForm <-> TileBakeParams, its hash in the map cache
// key, and the overlay/preset files. The scene, the cooker tools and
// the tests build their bake params through makeTerrainBakeParams so
// the game, the cache and the offline bakes can never drift.

namespace data {
class FormTypeRegistry;
}

namespace game {

// Every pupitre field onto its bake parameter (and back). Both lists
// live in TerrainGenTuning.cpp, in the same order.
void applyTerrainGenTuning(const data::TerrainGenTuningForm& form,
                           render::terraingen::TileBakeParams& params);
data::TerrainGenTuningForm
captureTerrainGenTuning(const render::terraingen::TileBakeParams& params);

// FNV-1a over every reflected field of the captured form: the
// generation component of mapBakeKey (an edit = a new map).
u64 hashTerrainGenTuning(const render::terraingen::TileBakeParams& params);

// The game's bake params from its two tuning records: seed, sea level,
// recurve (LandscapeTuningForm) + the pupitre, borders on.
render::terraingen::TileBakeParams
makeTerrainBakeParams(const data::LandscapeTuningForm& tuning,
                      const data::TerrainGenTuningForm& gen);

// The pupitre as a one-record plugin file (a §5 patch on the canonical
// record): the overlay data/mods/terrain-gen.toml the stack loads, or a
// named preset. `pluginName` seeds the plugin id (stable per name).
bool saveTerrainGenTuning(const data::TerrainGenTuningForm& form,
                          const std::filesystem::path& path,
                          const data::FormTypeRegistry& types,
                          const char* pluginName);
// Applies the file's record fields onto `out` (fields it lacks keep
// their value). False when the file has no pupitre record.
bool loadTerrainGenTuning(const std::filesystem::path& path,
                          const data::FormTypeRegistry& types,
                          data::TerrainGenTuningForm& out);

} // namespace game
