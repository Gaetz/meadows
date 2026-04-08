#include "DeferredScene.h"
#include "Graphics/Renderer.h"
#include <glm/gtx/transform.hpp>

DeferredScene::DeferredScene(graphics::Renderer* renderer)
    : GltfScene(renderer,
                "assets/armor/armor.gltf",
                glm::scale(Vec3(30.0f)) * glm::translate(Vec3(0.0f, 2.3f, 0.0f)))
{
    technique = std::make_unique<graphics::techniques::DeferredRenderingTechnique>();
    technique->init(renderer);
    initialCameraPos = Vec3(0.0f, 50.0f, 100.0f);
    animateLight     = false;
    loadKTXColorMap("assets/armor/colormap_rgba.ktx");
}

DeferredScene::~DeferredScene() {
    technique->cleanup(renderer->getContext()->getDevice());
}
