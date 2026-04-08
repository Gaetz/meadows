#pragma once

#include "IScene.h"
#include "Defines.h"
#include "Graphics/RenderObject.h"
#include "Graphics/Techniques/RainbowTechnique.h"

namespace graphics {
    class Renderer;
}

class RainbowScene : public IScene {
public:
    explicit RainbowScene(graphics::Renderer* renderer);
    ~RainbowScene() override;

    // IScene interface
    void update() override;
    void drawImGui() override;
    graphics::DrawContext& getDrawContext() override { return drawContext; }
    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const override;
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique*) override {}
    void onActivated(graphics::Renderer* renderer) override;

private:
    void drawDropletDiagram();

    graphics::Renderer* renderer { nullptr };
    uptr<graphics::techniques::RainbowTechnique> technique;
    graphics::DrawContext drawContext;
    graphics::Renderer* cachedRenderer { nullptr };
};
