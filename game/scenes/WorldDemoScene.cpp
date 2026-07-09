#include "game/scenes/WorldDemoScene.hpp"

#include "data/forms/CoreForms.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/Device.hpp"
#include "game/SceneSubmit.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/combat/Combat.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/StatsTuning.hpp"
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
    gameplay::registerStatsFormTypes(types);
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
    textureCache = std::make_unique<game::TextureCache>(
        engine.getDevice(), assetDb, engine.getJobSystem());

    // U1-03: loadPluginFile returns a core::Result — unwrap into the
    // optional members (absence stays a legal state for this demo scene).
    if (auto loaded =
            data::loadPluginFile(dataDir / "base" / "base.toml", types)) {
        basePlugin = std::move(*loaded);
    }
    if (auto loaded = data::loadPluginFile(
            dataDir / "mods" / "golden-blades" / "mod.toml", types)) {
        modPlugin = std::move(*loaded);
    }
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
    // Completion-queue drain point of the frame (§9 Phase 5): upload any asset
    // that finished decoding on a worker, flipping its handle to resident before
    // extractScene reads it. Runs right before the seam, on the main thread.
    textureCache->pumpUploads();

    // Loading gate (§7): while the visible set is still decoding, show only the
    // loading screen — never the world, so its placeholders stay hidden.
    if (loading) {
        if (textureCache->pendingCount() == 0) {
            loading = false; // everything resident: reveal the world this frame
        } else {
            drawLoadingScreen(renderer);
            return;
        }
    }

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
    // Strict ECS↔renderer seam (§9 Phase 5): extract a self-owning snapshot
    // from the world, then submit it. Same thread today; the split is what lets
    // submit move to a render thread later without touching gameplay.
    const game::RenderSnapshot snapshot = game::extractScene(world, *textureCache);
    game::submitSnapshot(snapshot, renderer);
}

void WorldDemoScene::drawLoadingScreen(render::SpriteRenderer& renderer) {
    const render::Camera2D& cam = engine.getCamera();
    const f32 aspect = static_cast<f32>(engine.getWindow().width()) /
                       static_cast<f32>(engine.getWindow().height());
    const f32 viewW = cam.viewHeight * aspect;

    // Opaque cover over the whole view (untextured → white fallback, tinted), so
    // the world behind it never shows. Centered on the camera, so it covers the
    // screen wherever the view is.
    renderer.draw({ .position = cam.position,
                    .size = { viewW, cam.viewHeight },
                    .tint = { 0.06f, 0.07f, 0.09f, 1.0f } });

    // Progress bar: the resident fraction of the assets we are waiting on.
    const u32 remaining = textureCache->pendingCount();
    const f32 done = loadTotal > 0
                         ? static_cast<f32>(loadTotal - remaining) /
                               static_cast<f32>(loadTotal)
                         : 1.0f;
    const f32 barW = viewW * 0.5f;
    const f32 barH = cam.viewHeight * 0.04f;

    renderer.draw({ .position = cam.position,
                    .size = { barW, barH },
                    .tint = { 0.18f, 0.19f, 0.24f, 1.0f } }); // track
    const f32 fillW = barW * done;
    if (fillW > 0.0f) {
        renderer.draw({ .position = { cam.position.x - (barW - fillW) * 0.5f,
                                      cam.position.y },
                        .size = { fillW, barH },
                        .tint = { 0.40f, 0.80f, 0.55f, 1.0f } }); // fill
    }
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

    // Refresh world-level stat resources whenever the plugin set changes (§5:
    // StatsTuningForm is a moddable Form, so tuning values can change per-rebuild).
    tuning  = gameplay::resolveStatsTuning(forms);
    derived = gameplay::DerivedStatRegistry {};
    gameplay::registerCoreDerivedStats(derived, tuning);

    // Kick the decode of every visible asset now and hold the world behind the
    // loading gate until they are resident (§7) — no startup pop-in.
    game::prewarmTextures(world, *textureCache);
    loadTotal = textureCache->pendingCount();
    loading = loadTotal > 0;
}

} // namespace game
