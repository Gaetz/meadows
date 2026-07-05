#pragma once

#include "engine/audio/Audio.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}

namespace game {

// H4 proof scene: an RmlUi document served through the plugin ui/ roots,
// rendered through the RHI on top of a plain clear. Mouse hover works
// (input routed); full input/data-binding is the "interfaces" vertical.
class UiDemoScene final : public Scene {
public:
    explicit UiDemoScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    void onEnter() override;
    void onExit() override;
    void update(f32 dt) override;
    bool ownsFrame() const override { return true; }
    void render(engine::FrameContext& frame) override;
    void drawUi() override;

private:
    engine::Engine* engine { nullptr };
    uptr<render::ShaderLibrary> shaders;
    // Fully qualified: game::ui (dev panels) shadows the engine's ::ui.
    ::ui::UiSystem uiSystem;
    audio::AudioSystem audioSystem; // H6 audible proof lives here too
    bool documentShown { false };
    str statusLine;
    u32 lastWidth { 0 };
    u32 lastHeight { 0 };
};

} // namespace game
