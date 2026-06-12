#pragma once
#include "GltfScene.h"
#include "Graphics/Techniques/ShadowMappingTechnique.h"

class ShadowScene : public GltfScene {
public:
    explicit ShadowScene(graphics::Renderer* renderer);
    ~ShadowScene() override;

    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const override { return technique.get(); }
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique*) override {}

private:
    uptr<graphics::techniques::ShadowMappingTechnique> technique;
};
