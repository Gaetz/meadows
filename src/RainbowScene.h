#pragma once

#include "IScene.h"
#include "Defines.h"
#include "Graphics/RenderObject.h"

namespace graphics {
    class Renderer;
}

namespace graphics::techniques {
    class IRenderingTechnique;
    class RainbowTechnique;
}

class RainbowScene : public IScene {
public:
    explicit RainbowScene(graphics::techniques::RainbowTechnique* technique);

    // IScene interface
    void update() override;
    void drawImGui() override;
    graphics::DrawContext& getDrawContext() override { return drawContext; }
    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const override;
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique* technique) override;
    void onActivated(graphics::Renderer* renderer) override;

private:
    void drawDropletDiagram();

    graphics::techniques::RainbowTechnique* technique { nullptr };
    graphics::DrawContext drawContext;

    graphics::Renderer* cachedRenderer { nullptr };
};
