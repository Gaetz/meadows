#pragma once
#include "Graphics/RenderObject.h"

namespace graphics {
    class Renderer;
    namespace techniques { class IRenderingTechnique; }
}

class IScene {
public:
    virtual ~IScene() = default;

    // Called every frame: clears and fills the DrawContext
    virtual void update() = 0;

    // ImGui debug panel
    virtual void drawImGui() = 0;

    // DrawContext access for the Renderer
    virtual graphics::DrawContext& getDrawContext() = 0;

    // Rendering technique
    virtual graphics::techniques::IRenderingTechnique* getRenderingTechnique() const = 0;
    virtual void setRenderingTechnique(graphics::techniques::IRenderingTechnique*) = 0;

    // Called when this scene becomes the active scene (camera setup, renderer config)
    virtual void onActivated(graphics::Renderer*) {}
};
