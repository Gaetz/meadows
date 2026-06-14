#include "game/scenes/WorldDemoScene.hpp"

#include "data/forms/CoreForms.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/Device.hpp"
#include "game/SceneSubmit.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/combat/Combat.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace game {

namespace {

// Stand-in ground texture until tiles become assets too.
rhi::TextureHandle createCheckerTexture(rhi::Device& device) {
    constexpr u32 size = 64;
    constexpr u32 cell = 8;
    vector<u32> pixels(size * size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool even = ((x / cell + y / cell) % 2) == 0;
            pixels[y * size + x] = even ? 0xFFFFFFFF : 0xFFC8C8C8;
        }
    }
    return device.createTexture({ .width = size, .height = size },
                                pixels.data());
}

} // namespace

void WorldDemoScene::onEnter() {
    dataDir = platform::executableDir() / "data";

    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerGameplayFormTypes(types);
    quest::registerQuestFormTypes(types);
    quest::registerDialogueFormTypes(types);
    world::registerCoreCategories(categories);
    world::registerCoreSpawners(spawner);
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);

    tags.registerTag("State.Dead");
    tags.registerTag("Cooldown.Strike");

    checker = createCheckerTexture(engine.getDevice());
    engine.getCamera().viewHeight = 12.0f;
    textureCache =
        std::make_unique<game::TextureCache>(engine.getDevice(), assetDb);

    basePlugin = data::loadPluginFile(dataDir / "base" / "base.toml", types);
    modPlugin = data::loadPluginFile(
        dataDir / "mods" / "golden-blades" / "mod.toml", types);
    if (!basePlugin) {
        LOG_CRITICAL("Base plugin failed to load; nothing to show");
    }
    rebuild();
}

void WorldDemoScene::onExit() {
    // Free GPU resources while the device is alive (onExit runs from the main
    // thread during applyPending, before any teardown).
    textureCache.reset();
    if (checker.id != 0) {
        engine.getDevice().destroyTexture(checker);
    }
}

void WorldDemoScene::update(f32 dt) {
    // GAS tick (§6): advance active effects + refresh life state on every actor.
    world.handle()
        .query<gameplay::AttributeSet, gameplay::AbilitySystem>()
        .each([&](flecs::entity, gameplay::AttributeSet& set,
                  gameplay::AbilitySystem& system) {
            gameplay::tickEffects(set, system, dt, tags);
            gameplay::updateLifeState(system, tags);
        });
}

void WorldDemoScene::draw(render::SpriteRenderer& renderer) {
    for (i32 y = -5; y < 5; ++y) {
        for (i32 x = -8; x < 8; ++x) {
            const f32 shade =
                0.85f + 0.15f * static_cast<f32>((x * 7 + y * 13 + 60) % 5) / 4.0f;
            renderer.draw({
                .position = { static_cast<f32>(x) + 0.5f,
                              static_cast<f32>(y) + 0.5f },
                .tint = { 0.35f * shade, 0.55f * shade, 0.30f * shade, 1.0f },
                .texture = checker,
            });
        }
    }
    game::submitScene(world, *textureCache, renderer);
}

void WorldDemoScene::rebuild() {
    vector<const data::Plugin*> loadOrder;
    if (basePlugin) {
        loadOrder.push_back(&*basePlugin);
    }
    if (modEnabled && modPlugin) {
        loadOrder.push_back(&*modPlugin);
    }

    forms = data::FormDatabase {};
    report = data::resolve(loadOrder, types, forms);

    assetDb = assets::AssetDatabase {};
    for (const data::Plugin* plugin : loadOrder) {
        for (const data::AssetEntry& asset : plugin->assets) {
            assetDb.add(asset.id, plugin->baseDir, asset.path);
        }
    }
    textureCache->clear();

    model = world::WorldModel::build(forms);

    if (!cellLoader) {
        cellLoader.emplace(world, forms, model, spawner, categories);
    } else {
        cellLoader->unloadAll();
    }
    cellLoader->loadAll();

    LOG_INFO("World rebuilt: {} plugins, {} forms, {} cells, {} conflicts",
             loadOrder.size(), forms.count(), model.cells().size(),
             report.conflicts.size());
}

} // namespace game
