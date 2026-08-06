#include "game/RendererAssets.hpp"

#include <algorithm>
#include <filesystem>

#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "engine/assets/Image.hpp"

namespace game {

assets::AssetDatabase buildAssetDatabase(const data::PluginStack& stack) {
    assets::AssetDatabase assetDb;
    for (const data::Plugin& plugin : stack.plugins) {
        for (const data::AssetEntry& entry : plugin.assets) {
            assetDb.add(entry.id, plugin.baseDir, entry.path);
        }
    }
    return assetDb;
}

void applyCookedTerrainPaths(render::RendererConfig& config,
                             const data::FormDatabase& forms,
                             const assets::AssetDatabase& assetDb) {
    const data::LandscapeTuningForm tuning =
        data::resolveLandscapeTuning(forms);
    const auto pathOf = [&](const core::Guid& id) -> str {
        if (!id.isValid()) {
            return {};
        }
        const auto path = assetDb.resolve(id);
        return path ? path->string() : str {};
    };
    config.terrainAlbedoPath = pathOf(tuning.terrainAlbedoArray);
    config.terrainNormalPath = pathOf(tuning.terrainNormalArray);
    config.terrainOrmPath = pathOf(tuning.terrainOrmArray);
    config.terrainHeightPath = pathOf(tuning.terrainHeightArray);
}

void loadTreeBark(rhi::Device& device, render::WorldRenderer& renderer,
                  const assets::AssetDatabase& assetDb) {
    const auto oakGuid = core::Guid::fromString(
        "52035a3f-8246-419a-aa69-a686b0c2e834");
    const auto pineGuid = core::Guid::fromString(
        "8244825d-a9a7-4a30-a8e7-996670193884");
    const auto oakPath = oakGuid ? assetDb.resolve(*oakGuid) : std::nullopt;
    const auto pinePath =
        pineGuid ? assetDb.resolve(*pineGuid) : std::nullopt;
    // Per bark: albedo + a PACKED normal-height image (nor_gl RGB,
    // displacement in alpha — sibling files of the diffuse).
    const auto loadBark = [](const std::filesystem::path& diffPath,
                             render::VegetationSystem::BarkImage& albedo,
                             render::VegetationSystem::BarkImage&
                                 nrmHeight) {
        auto diff = assets::loadImageFile(diffPath);
        if (!diff) {
            return false;
        }
        str name = diffPath.filename().string();
        const auto sub = [&](const char* tag) {
            str s = name;
            s.replace(s.find("_diff_"), 6, tag);
            return diffPath.parent_path() / s;
        };
        auto nor = assets::loadImageFile(sub("_nor_gl_"));
        if (nor) {
            if (const auto disp = assets::loadImageFile(sub("_disp_"));
                disp && disp->width == nor->width &&
                disp->height == nor->height) {
                const size_t pixels =
                    static_cast<size_t>(nor->width) * nor->height;
                // Min-max stretch: some disp maps (the oak) sit in a
                // soft mid band — normalized, both the bark parallax
                // and the SSDM warp bite.
                u8 lo = 255;
                u8 hi = 0;
                for (size_t p = 0; p < pixels; ++p) {
                    const u8 v = disp->pixels[p * 4 + 0];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                const f32 scale =
                    hi > lo ? 255.0f / static_cast<f32>(hi - lo) : 1.0f;
                for (size_t p = 0; p < pixels; ++p) {
                    nor->pixels[p * 4 + 3] = static_cast<u8>(glm::clamp(
                        static_cast<f32>(disp->pixels[p * 4 + 0] - lo) *
                            scale,
                        0.0f, 255.0f));
                }
            }
            nrmHeight = { nor->width, nor->height,
                          std::move(nor->pixels) };
        }
        albedo = { diff->width, diff->height, std::move(diff->pixels) };
        return true;
    };
    render::VegetationSystem::BarkImage oakA, oakN, pineA, pineN;
    if (oakPath && pinePath && loadBark(*oakPath, oakA, oakN) &&
        loadBark(*pinePath, pineA, pineN)) {
        renderer.setVegetationBark(device, std::move(oakA),
                                   std::move(oakN), std::move(pineA),
                                   std::move(pineN));
    }
}

} // namespace game
