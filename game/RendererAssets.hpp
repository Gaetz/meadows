#pragma once

#include "data/forms/FormQuery.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/render/WorldRenderer.hpp"

namespace data {
struct PluginStack;
}

namespace assets {
struct Image;
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

// Packs a displacement map into the normal image's alpha, MEAN-
// CENTERED: source disp maps carry arbitrary bias (bark_brown_02 means
// 0.26 where jolcham sat at 0.51) and the relief encode assumes the
// surface level is 0.5 — without centering, a biased map displaces the
// whole surface uniformly (the "texture zooms" symptom) instead of
// popping crests and digging pits. No-op on dimension mismatch.
void packDispIntoNormalAlpha(assets::Image& normal,
                             const assets::Image& disp);

// Loads the tree bark texture sets (oak + spruce, albedo + packed
// mean-centered normal-height alpha) and hands them to the vegetation
// system. Call AFTER renderer.create().
void loadTreeBark(rhi::Device& device, render::WorldRenderer& renderer,
                  const assets::AssetDatabase& assetDb);

} // namespace game
