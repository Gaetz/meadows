#include "ShadowScene.h"
#include "Graphics/Renderer.h"
#include "Graphics/LoadedGLTF.h"
#include "BasicServices/Log.h"
#include <glm/gtc/constants.hpp>

ShadowScene::ShadowScene(graphics::Renderer* renderer)
    : renderer(renderer) {

    // Load the shadow scene model
    std::string modelPath = "assets/vulkanscene_shadow.gltf";
    auto loadedModel = graphics::loadGltf(renderer, modelPath);

    if (loadedModel.has_value()) {
        sceneModel = *loadedModel;
        services::Log::Info("ShadowScene: Loaded model from %s", modelPath.c_str());
    } else {
        services::Log::Error("ShadowScene: Failed to load model from %s", modelPath.c_str());
    }
}

ShadowScene::~ShadowScene() {
    sceneModel.reset();
}

void ShadowScene::update(float deltaTime) {
    // Clear draw context
    drawContext.opaqueSurfaces.clear();
    drawContext.transparentSurfaces.clear();

    // Animate light position
    if (animateLight) {
        lightAngle += deltaTime * 0.5f;  // Rotate 0.5 radians per second
        if (lightAngle > glm::two_pi<float>()) {
            lightAngle -= glm::two_pi<float>();
        }

        // Orbit light around scene
        float radius = 50.0f;
        lightPos.x = std::cos(lightAngle) * radius;
        lightPos.z = std::sin(lightAngle) * radius;
    }

    // Fill draw context with scene model
    if (sceneModel) {
        sceneModel->draw(Mat4{1.f}, drawContext);
    }
}
