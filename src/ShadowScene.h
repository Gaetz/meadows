#pragma once

#include "Defines.h"
#include "Graphics/Types.h"
#include "Graphics/RenderObject.h"
#include "Graphics/LoadedGLTF.h"

namespace graphics {
    class Renderer;
}

class ShadowScene {
public:
    explicit ShadowScene(graphics::Renderer* renderer);
    ~ShadowScene();

    void update(float deltaTime);

    graphics::DrawContext& getDrawContext() { return drawContext; }
    const graphics::DrawContext& getDrawContext() const { return drawContext; }

    // Light animation
    void setAnimateLight(bool animate) { animateLight = animate; }
    bool isAnimatingLight() const { return animateLight; }
    Vec3 getLightPosition() const { return lightPos; }

private:
    graphics::Renderer* renderer;

    // Scene model
    sptr<graphics::LoadedGLTF> sceneModel;

    // Draw context - filled during update()
    graphics::DrawContext drawContext;

    // Light animation
    bool animateLight { true };
    float lightAngle { 0.0f };
    Vec3 lightPos { 0.0f, 50.0f, 25.0f };
};
