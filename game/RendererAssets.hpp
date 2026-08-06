#pragma once

#include "data/forms/FormQuery.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/render/WorldRenderer.hpp"

namespace data {
struct PluginStack;
}

namespace game {

// Shared data→renderer resource wiring (the §4 seam: the renderer never
// sees Forms nor the VFS — the SCENE resolves guids to file paths and
// hands plain data over). One implementation for every scene that
// stands up a WorldRenderer (LandscapeScene, TreeCreationScene): a
// scene that skips this shows placeholder terrain materials and bare
// procedural trunks.

// Guid → file map, layered per plugin order (later plugins override).
assets::AssetDatabase buildAssetDatabase(const data::PluginStack& stack);

// Resolves the LandscapeTuningForm's cooked splat-array guids into the
// renderer config's file paths (empty when unresolved — TerrainSystem
// falls back to flat placeholders with a warning).
void applyCookedTerrainPaths(render::RendererConfig& config,
                             const data::FormDatabase& forms,
                             const assets::AssetDatabase& assetDb);

// Loads the tree bark texture sets (oak + spruce, albedo + packed
// normal-height with the displacement min-max-normalized into the
// alpha) and hands them to the vegetation system. Call AFTER
// renderer.create().
void loadTreeBark(rhi::Device& device, render::WorldRenderer& renderer,
                  const assets::AssetDatabase& assetDb);

} // namespace game
