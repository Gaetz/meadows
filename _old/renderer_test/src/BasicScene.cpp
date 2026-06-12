#include "BasicScene.h"
#include "Graphics/Renderer.h"

BasicScene::BasicScene(graphics::Renderer* renderer)
    : GltfScene(renderer, "assets/structure.glb")
{
    technique = std::make_unique<graphics::techniques::BasicTechnique>();
    technique->init(renderer);
    animateLight = false;
}

BasicScene::~BasicScene() {
    technique->cleanup(renderer->getContext()->getDevice());
}
