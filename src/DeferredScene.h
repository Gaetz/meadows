#pragma once
#include "GltfScene.h"
#include "Graphics/Techniques/DeferredRenderingTechnique.h"

class DeferredScene : public GltfScene {
public:
    explicit DeferredScene(graphics::Renderer* renderer);
    ~DeferredScene() override;

    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const override { return technique.get(); }
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique*) override {}

private:
    uptr<graphics::techniques::DeferredRenderingTechnique> technique;
};
