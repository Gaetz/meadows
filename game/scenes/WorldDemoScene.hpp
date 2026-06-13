#pragma once

#include <filesystem>
#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Record.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/Engine.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/ecs/World.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"
#include "game/TextureCache.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldModel.hpp"

namespace game {

// Base for the demo scenes: owns an INDEPENDENT game world (its own ecs::World,
// FormDatabase, asset DB, tag vocabulary…) loaded from the base plugin, renders
// it (ground + the ECS scene), and runs the GAS tick. Derived scenes add their
// own tool panel in drawUi(). This is the isolation the scene stack buys — each
// demo is self-contained.
class WorldDemoScene : public Scene {
public:
    explicit WorldDemoScene(engine::Engine& engine) : engine(engine) {}

    void onEnter() override;
    void onExit() override;
    void update(f32 dt) override;
    void draw(render::SpriteRenderer& renderer) override;

protected:
    void rebuild(); // re-resolve plugins → assets → world model → load cells

    engine::Engine& engine;
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
    rhi::TextureHandle checker {};
};

} // namespace game
