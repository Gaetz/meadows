#include <cmath>
#include <unordered_map>

#include <imgui.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/Engine.hpp"
#include "engine/Game.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Image.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/SpriteRenderer.hpp"
#include "engine/rhi/Device.hpp"

namespace {

const core::Guid kSwordFormId =
    *core::Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482");

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

        checker = createCheckerTexture(engine->getDevice());
        engine->getCamera().viewHeight = 8.0f;

        basePlugin = data::loadPluginFile(dataDir / "base" / "base.toml", types);
        modPlugin = data::loadPluginFile(
            dataDir / "mods" / "golden-blades" / "mod.toml", types);
        if (!basePlugin) {
            LOG_CRITICAL("Base plugin failed to load; nothing to show");
        }
        loadWorld();
    }

    void update(f32 dt) override {
        time += dt;
    }

    void draw(render::SpriteRenderer& renderer) override {
        for (i32 y = -4; y < 4; ++y) {
            for (i32 x = -7; x < 7; ++x) {
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

        if (sword) {
            renderer.draw({
                .position = { 0.0f, 0.15f * std::sin(time * 2.0f) },
                .size = { 2.5f, 2.5f },
                .rotation = 0.15f * std::sin(time * 0.7f),
                .texture = swordTexture,
            });
        }
    }

    void drawUi() override {
        ImGui::Begin("True Adventurer - data model demo");
        ImGui::Text("%.1f fps", ImGui::GetIO().Framerate);
        ImGui::Separator();

        if (modPlugin &&
            ImGui::Checkbox("Enable 'golden-blades' mod", &modEnabled)) {
            loadWorld(); // full §5 re-resolution, live
        }

        if (sword) {
            ImGui::Text("Resolved WeaponForm (%s):", sword->editorId.c_str());
            ImGui::BulletText("displayName: %s", sword->displayName.c_str());
            ImGui::BulletText("damage: %.1f", sword->damage);
            ImGui::BulletText("weight: %.1f (base value, no mod touches it)",
                              sword->weight);
            ImGui::BulletText("goldValue: %d", sword->goldValue);
        } else {
            ImGui::TextColored({ 1, 0.4f, 0.4f, 1 }, "Sword form not found");
        }

        ImGui::Separator();
        ImGui::Text("Resolve report: %u forms, %u records, %u conflicts",
                    report.formsCreated, report.recordsApplied,
                    static_cast<u32>(report.conflicts.size()));
        for (const data::FieldConflict& conflict : report.conflicts) {
            str writers;
            for (size_t i = 0; i < conflict.writers.size(); ++i) {
                writers += (i ? " -> " : "") + conflict.writers[i];
            }
            ImGui::BulletText("%s.%s: %s (last wins)",
                              conflict.typeName.c_str(),
                              conflict.fieldName.c_str(), writers.c_str());
        }
        ImGui::End();
    }

    void close() override {}

private:
    void loadWorld() {
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

        // The texture behind an asset guid may have changed: drop the cache.
        for (auto& [path, texture] : textureCache) {
            engine->getDevice().destroyTexture(texture);
        }
        textureCache.clear();

        sword = forms.find<data::WeaponForm>(kSwordFormId);
        swordTexture = sword ? textureFor(sword->sprite)
                             : rhi::TextureHandle {};
        LOG_INFO("World loaded: {} plugins, {} forms, {} conflicts",
                 loadOrder.size(), forms.count(), report.conflicts.size());
    }

    rhi::TextureHandle textureFor(const core::Guid& assetId) {
        const auto path = assetDb.resolve(assetId);
        if (!path) {
            LOG_WARN("No asset for guid {}", assetId.toString());
            return {};
        }
        if (const auto it = textureCache.find(path->string());
            it != textureCache.end()) {
            return it->second;
        }
        const auto image = assets::loadImageFile(*path);
        if (!image) {
            return {};
        }
        const auto texture = engine->getDevice().createTexture(
            { .width = image->width, .height = image->height,
              .filter = rhi::FilterMode::Nearest },
            image->pixels.data());
        textureCache.emplace(path->string(), texture);
        return texture;
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
    std::unordered_map<str, rhi::TextureHandle> textureCache;

    const data::WeaponForm* sword { nullptr };
    rhi::TextureHandle swordTexture {};
    rhi::TextureHandle checker {};
    f32 time { 0.0f };
};

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    TrueAdventurer game;
    return engine::Engine::run({ .title = "True Adventurer" }, game);
}
