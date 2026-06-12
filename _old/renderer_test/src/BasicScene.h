#pragma once
#include "GltfScene.h"
#include "Graphics/Techniques/BasicTechnique.h"

class BasicScene : public GltfScene {
public:
    explicit BasicScene(graphics::Renderer* renderer);
    ~BasicScene() override;

    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const override { return technique.get(); }
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique*) override {}

private:
    uptr<graphics::techniques::BasicTechnique> technique;
};
