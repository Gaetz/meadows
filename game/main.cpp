#include <optional>

#include <imgui.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/Engine.hpp"
#include "engine/Game.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/Log.hpp"
#include "engine/ecs/World.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/Device.hpp"
#include "game/SceneSubmit.hpp"
#include "game/TextureCache.hpp"
#include "game/WorldEditor.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/combat/Combat.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

namespace {

const core::Guid kStrikeAbility =
    *core::Guid::fromString("ab000000-0000-4000-8000-000000000001");

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

class TrueAdventurer final : public engine::Game {
public:
    void init(engine::Engine& engineContext) override {
        engine = &engineContext;
        dataDir = platform::executableDir() / "data";

        data::registerCoreFormTypes(types);
        world::registerWorldFormTypes(types);
        gameplay::registerGameplayFormTypes(types);
        world::registerCoreCategories(categories);
        world::registerCoreSpawners(spawner);
        world::registerSceneComponents(world);
        gameplay::registerGameplayComponents(world);

        // Gameplay tag vocabulary (a data manifest will feed this later).
        tags.registerTag("State.Dead");
        tags.registerTag("Cooldown.Strike");

        checker = createCheckerTexture(engine->getDevice());
        engine->getCamera().viewHeight = 12.0f;

        textureCache = std::make_unique<game::TextureCache>(
            engine->getDevice(), assetDb);
        editor = std::make_unique<game::WorldEditor>(world, forms, categories,
                                                     spawner);

        basePlugin =
            data::loadPluginFile(dataDir / "base" / "base.toml", types);
        modPlugin = data::loadPluginFile(
            dataDir / "mods" / "golden-blades" / "mod.toml", types);
        if (!basePlugin) {
            LOG_CRITICAL("Base plugin failed to load; nothing to show");
        }
        rebuild();
    }

    void update(f32 dt) override {
        time += dt;
        // GAS tick (§6): advance active effects (durations, periodics, cooldowns)
        // on every actor, then refresh derived life state. Ordered C++ update —
        // no flecs pipeline yet.
        world.handle()
            .query<gameplay::AttributeSet, gameplay::AbilitySystem>()
            .each([&](flecs::entity, gameplay::AttributeSet& set,
                      gameplay::AbilitySystem& system) {
                gameplay::tickEffects(set, system, dt, tags);
                gameplay::updateLifeState(system, tags);
            });
    }

    void draw(render::SpriteRenderer& renderer) override {
        // Ground tiles (a flat checkered floor under the scene).
        for (i32 y = -5; y < 5; ++y) {
            for (i32 x = -8; x < 8; ++x) {
                const f32 shade =
                    0.85f +
                    0.15f * static_cast<f32>((x * 7 + y * 13 + 60) % 5) / 4.0f;
                renderer.draw({
                    .position = { static_cast<f32>(x) + 0.5f,
                                  static_cast<f32>(y) + 0.5f },
                    .tint = { 0.35f * shade, 0.55f * shade, 0.30f * shade,
                              1.0f },
                    .texture = checker,
                });
            }
        }

        // The ECS scene: the single ECS→rhi seam (§2.6).
        game::submitScene(world, *textureCache, renderer);
    }

    void drawUi() override {
        ImGui::Begin("True Adventurer - Phase 2 world");
        ImGui::Text("%.1f fps", ImGui::GetIO().Framerate);
        ImGui::Separator();

        if (modPlugin &&
            ImGui::Checkbox("Enable 'golden-blades' mod", &modEnabled)) {
            rebuild(); // full §5 re-resolution; discards live editor edits
        }
        ImGui::TextWrapped(
            "The mod patches the sword (gold + stronger), moves Sword_A and "
            "disables Sword_B - all field-level patches on records.");

        ImGui::Separator();
        ImGui::Text("%u forms, %zu cells, %zu conflicts", forms.count(),
                    model.cells().size(), report.conflicts.size());
        for (const data::FieldConflict& conflict : report.conflicts) {
            ImGui::BulletText("%s.%s", conflict.typeName.c_str(),
                              conflict.fieldName.c_str());
        }
        ImGui::End();

        editor->drawUi();
        drawCombatPanel();
    }

    void close() override {
        // Free GPU resources while the device is still alive (close() runs
        // before engine teardown); reset the cache so its dtor does not touch a
        // dead device.
        textureCache.reset();
        if (checker.id != 0) {
            engine->getDevice().destroyTexture(checker);
        }
    }

private:
    void rebuild() {
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
        textureCache->clear(); // an asset guid may now map to a different file

        model = world::WorldModel::build(forms);

        if (!cellLoader) {
            cellLoader.emplace(world, forms, model, spawner, categories);
        } else {
            cellLoader->unloadAll();
        }
        cellLoader->loadAll();

        // Point the editor at the first cell (the demo has a single one).
        if (!model.cells().empty()) {
            const data::FormHandle cellHandle = model.cells().front();
            const data::Form* cellForm = forms.get(cellHandle);
            editor->setActiveCell(cellLoader->cellEntity(cellHandle),
                                  cellForm ? cellForm->id : core::Guid {});
        } else {
            editor->setActiveCell({}, {});
        }

        LOG_INFO("World rebuilt: {} plugins, {} forms, {} cells, {} conflicts",
                 loadOrder.size(), forms.count(), model.cells().size(),
                 report.conflicts.size());
    }

    // Debug panel: every actor's AbilitySystem, with a Strike button that
    // activates the attack ability on it (combat = abilities + effects, §6).
    void drawCombatPanel() {
        const gameplay::AbilityForm* strike =
            forms.find<gameplay::AbilityForm>(kStrikeAbility);
        const auto deadTag = tags.find("State.Dead");

        ImGui::Begin("Combat (GAS debug)");
        if (!strike) {
            ImGui::TextDisabled("No Strike ability in the database.");
            ImGui::End();
            return;
        }
        ImGui::TextWrapped("'Strike' applies a 10-damage effect (1s cooldown). "
                           "Health 0 grants State.Dead.");
        ImGui::Separator();

        world.handle()
            .query<gameplay::AttributeSet, gameplay::AbilitySystem>()
            .each([&](flecs::entity entity, gameplay::AttributeSet& set,
                      gameplay::AbilitySystem& system) {
                str name = "actor";
                if (const world::RefId* refId = entity.try_get<world::RefId>()) {
                    if (const data::Form* base = forms.get(refId->base);
                        base && !base->editorId.empty()) {
                        name = base->editorId;
                    }
                }
                const f32 health =
                    gameplay::currentValueOf(system, gameplay::attr("health"));
                const f32 maxHealth =
                    gameplay::currentValueOf(system, gameplay::attr("maxHealth"));
                const bool dead = deadTag && system.tags.has(*deadTag);

                ImGui::PushID(static_cast<int>(entity.id()));
                ImGui::Text("%s%s  %.0f / %.0f", name.c_str(),
                            dead ? " (DEAD)" : "", health, maxHealth);
                ImGui::ProgressBar(maxHealth > 0.0f ? health / maxHealth : 0.0f,
                                   ImVec2(-1.0f, 0.0f));
                if (ImGui::Button("Strike")) {
                    gameplay::performAttack(*strike, set, system, set, system,
                                            { forms, tags });
                }
                ImGui::PopID();
                ImGui::Separator();
            });
        ImGui::End();
    }

    engine::Engine* engine { nullptr };
    std::filesystem::path dataDir;
    data::FormTypeRegistry types;

    std::optional<data::Plugin> basePlugin;
    std::optional<data::Plugin> modPlugin;
    bool modEnabled { false };

    data::FormDatabase forms;
    data::ResolveReport report;
    assets::AssetDatabase assetDb;
    world::WorldModel model;

    ecs::World world;
    world::FormCategoryRegistry categories;
    world::Spawner spawner;
    gameplay::GameplayTagRegistry tags;
    std::optional<world::CellLoader> cellLoader;

    uptr<game::TextureCache> textureCache;
    uptr<game::WorldEditor> editor;

    rhi::TextureHandle checker {};
    f32 time { 0.0f };
};

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    TrueAdventurer game;
    return engine::Engine::run({ .title = "True Adventurer" }, game);
}
