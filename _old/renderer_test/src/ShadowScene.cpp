#include "ShadowScene.h"
#include "Graphics/Renderer.h"
#include <glm/gtx/transform.hpp>

ShadowScene::ShadowScene(graphics::Renderer* renderer)
    : GltfScene(renderer, "assets/vulkanscene_shadow.gltf")
{
    technique = std::make_unique<graphics::techniques::ShadowMappingTechnique>();
    technique->init(renderer);
    initialCameraPos   = Vec3(0.0f, 5.0f, 10.0f);
    initialCameraPitch = glm::radians(-15.0f);
    initialCameraYaw   = 0.0f;
    animateLight       = true;
}

ShadowScene::~ShadowScene() {
    technique->cleanup(renderer->getContext()->getDevice());
}
