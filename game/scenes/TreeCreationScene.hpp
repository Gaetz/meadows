#pragma once

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/forms/LandscapeForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "engine/core/FrameProbe.hpp"
#include "engine/render/AtmosphereParams.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/WorldRenderer.hpp"
#include "game/Scene.hpp"
#include "game/scenes/WeatherController.hpp"

namespace engine {
class Engine;
}

namespace game {

// The tree-type authoring scene — the WorldRenderer's first partial-config
// consumer (docs/RENDERING.md §7 R5): flat terrain + grass + sky + one
// showcased tree; water/GI/froxels/occlusion never created. Independent of
// LandscapeScene (own plugin stack); the Edit-mode scene strip REPLACES the
// world with it (and back), no warm overlay.
//
// A tree TYPE is an ordinary record of LobeTreeTuningForm or
// ColonizedTreeTuningForm (the algorithm IS the record type) with its own
// guid + editorId, persisted as the mods/tree-types.toml plugin layer —
// the singleton tuning records driving the current forest are untouched.
// Wiring the types into the forest scatter is a follow-up chantier.
class TreeCreationScene final : public Scene {
public:
    explicit TreeCreationScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    void onEnter() override;
    void onExit() override;
    void update(f32 dt) override;
    bool ownsFrame() const override { return true; }
    void render(engine::FrameContext& frame) override;
    void drawUi() override;

private:
    // One library entry: exactly one of the two forms is live, per
    // `colonized` (the other keeps defaults for a later algo switch).
    struct TreeType {
        core::Guid id;
        str name;
        bool colonized { true };
        data::LobeTreeTuningForm lobes;
        data::ColonizedTreeTuningForm colonizedParams;
    };

    void loadLibrary();                // resolved DB -> `library` (+seed)
    void saveLibrary();                // library -> mods/tree-types.toml
    void applySelected();              // selected params -> generators
    void captureIntoSelected();        // live generators -> selected type

    engine::Engine* engine { nullptr };
    data::FormTypeRegistry formTypes;
    data::PluginStack pluginStack;
    data::FormDatabase forms;

    render::WorldRenderer renderer;
    render::FlyCamera flyCamera;
    render::AtmosphereParams atmos;
    WeatherController weather; // the top bar's weather selector
    core::FrameProbe frameProbe;
    f32 timeSeconds { 0.0f };
    f32 windTime { 0.0f };

    vector<TreeType> library;
    i32 selected { 0 };
    char nameBuffer[64] {};
    bool renderReady { false };
};

} // namespace game
