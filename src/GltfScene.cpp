#include "GltfScene.h"
#include "Graphics/Renderer.h"
#include "Graphics/RenderObject.h"
#include "Graphics/LoadedGLTF.h"
#include "Graphics/KTXLoader.h"
#include "Graphics/Pipelines/GLTFMetallicRoughness.h"
#include "Graphics/Techniques/IRenderingTechnique.h"
#include "Graphics/Techniques/ShadowMappingTechnique.h"
#include "Graphics/Techniques/DeferredRenderingTechnique.h"
#include "BasicServices/Log.h"
#include <imgui.h>

GltfScene::GltfScene(graphics::Renderer* renderer, const str& gltfPath, Mat4 modelTransform)
    : renderer(renderer), modelTransform(modelTransform)
{
    vector<graphics::DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        { vk::DescriptorType::eCombinedImageSampler, 3 },
        { vk::DescriptorType::eUniformBuffer, 3 },
    };
    vk::Device device = renderer->getContext()->getDevice();
    descriptorPool = graphics::DescriptorAllocatorGrowable(device, 100, sizes);
    initializeDefaultMaterial();

    if (!gltfPath.empty()) {
        auto loaded = graphics::loadGltf(renderer, gltfPath);
        if (loaded.has_value()) {
            model = *loaded;
            services::Log::Info("GltfScene: loaded '%s'", gltfPath.c_str());
        } else {
            services::Log::Error("GltfScene: failed to load '%s'", gltfPath.c_str());
        }
    }
}

GltfScene::~GltfScene() {
    nodes.clear();
    topNodes.clear();
    materialConstantsBuffer.destroy();
    ktxMaterialBuffer.destroy();
    if (ktxColorMap.has_value()) {
        ktxColorMap->destroy(renderer->getContext());
        ktxColorMap.reset();
    }
}

void GltfScene::initializeDefaultMaterial() {
    using namespace graphics::pipelines;

    vk::Device device = renderer->getContext()->getDevice();

    materialConstantsBuffer = graphics::Buffer(
        renderer->getContext(),
        sizeof(GLTFMetallicRoughness::MaterialConstants),
        vk::BufferUsageFlagBits::eUniformBuffer,
        VMA_MEMORY_USAGE_CPU_TO_GPU
    );

    auto* constants = static_cast<GLTFMetallicRoughness::MaterialConstants*>(
        materialConstantsBuffer.info.pMappedData
    );
    constants->colorFactors = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    constants->metalRoughFactors = Vec4(0.0f, 0.5f, 0.0f, 0.0f);

    GLTFMetallicRoughness::MaterialResources resources;
    resources.colorImage = renderer->whiteImage;
    resources.colorSampler = renderer->defaultSamplerLinear;
    resources.metalRoughImage = renderer->whiteImage;
    resources.metalRoughSampler = renderer->defaultSamplerLinear;
    resources.dataBuffer = materialConstantsBuffer.buffer;
    resources.dataBufferOffset = 0;

    defaultMaterial = renderer->metalRoughMaterial.writeMaterial(
        device,
        graphics::MaterialPass::MainColor,
        resources,
        &descriptorPool
    );

    hasDefaultMaterial = true;
}

void GltfScene::update() {
    drawContext.opaqueSurfaces.clear();
    drawContext.transparentSurfaces.clear();

    // Draw glTF model if present
    if (model) {
        model->draw(modelTransform, drawContext);
    }

    // Also draw any manually added nodes
    for (auto& node : topNodes) {
        node->refreshTransform(Mat4(1.0f));
    }
    for (auto& node : topNodes) {
        node->draw(Mat4(1.0f), drawContext);
    }
}

void GltfScene::onActivated(graphics::Renderer* r) {
    r->mainCamera.position = initialCameraPos;
    r->mainCamera.pitch    = initialCameraPitch;
    r->mainCamera.yaw      = initialCameraYaw;
    r->setAnimateLight(animateLight);
}

void GltfScene::setRenderingTechnique(graphics::techniques::IRenderingTechnique* technique) {
    renderingTechnique = technique;
}

void GltfScene::loadKTXColorMap(const str& colorKtxPath) {
    using namespace graphics::pipelines;

    auto loaded = graphics::loadKTXImage(renderer, colorKtxPath);
    if (!loaded.has_value()) {
        services::Log::Error("GltfScene: failed to load KTX '%s'", colorKtxPath.c_str());
        return;
    }
    ktxColorMap = std::move(*loaded);
    services::Log::Info("GltfScene: loaded KTX color map '%s'", colorKtxPath.c_str());

    ktxMaterialBuffer = graphics::Buffer(
        renderer->getContext(),
        sizeof(GLTFMetallicRoughness::MaterialConstants),
        vk::BufferUsageFlagBits::eUniformBuffer,
        VMA_MEMORY_USAGE_CPU_TO_GPU
    );

    auto* constants = static_cast<GLTFMetallicRoughness::MaterialConstants*>(
        ktxMaterialBuffer.info.pMappedData
    );
    constants->colorFactors = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    constants->metalRoughFactors = Vec4(0.0f, 0.5f, 0.0f, 0.0f);

    GLTFMetallicRoughness::MaterialResources resources;
    resources.colorImage = *ktxColorMap;
    resources.colorSampler = renderer->defaultSamplerLinear;
    resources.metalRoughImage = renderer->whiteImage;
    resources.metalRoughSampler = renderer->defaultSamplerLinear;
    resources.dataBuffer = ktxMaterialBuffer.buffer;
    resources.dataBufferOffset = 0;

    vk::Device device = renderer->getContext()->getDevice();
    auto* pool = model ? &model->descriptorPool : &descriptorPool;
    auto material = renderer->metalRoughMaterial.writeMaterial(
        device,
        graphics::MaterialPass::MainColor,
        resources,
        pool
    );

    if (model) {
        for (auto& [name, mesh] : model->meshes) {
            for (auto& surface : mesh->surfaces) {
                surface.material = std::make_shared<graphics::GLTFMaterial>();
                surface.material->data = material;
            }
        }
        services::Log::Info("GltfScene: applied KTX material to all model surfaces");
    }
}

sptr<graphics::Node> GltfScene::addNode(const str& name) {
    auto node = std::make_shared<graphics::Node>();
    node->localTransform = Mat4(1.0f);
    node->worldTransform = Mat4(1.0f);
    nodes[name] = node;
    topNodes.push_back(node);
    return node;
}

sptr<graphics::MeshNode> GltfScene::addMeshNode(const str& name, sptr<graphics::MeshAsset> mesh) {
    auto meshNode = std::make_shared<graphics::MeshNode>();
    meshNode->mesh = mesh;
    meshNode->localTransform = Mat4(1.0f);
    meshNode->worldTransform = Mat4(1.0f);

    for (auto& surface : mesh->surfaces) {
        if (!surface.material) {
            surface.material = std::make_shared<graphics::GLTFMaterial>();
            surface.material->data = defaultMaterial;
        }
    }

    nodes[name] = meshNode;
    topNodes.push_back(meshNode);
    return meshNode;
}

void GltfScene::setParent(const str& childName, const str& parentName) {
    auto childIt = nodes.find(childName);
    auto parentIt = nodes.find(parentName);
    if (childIt == nodes.end() || parentIt == nodes.end()) return;

    sptr<graphics::Node> child = childIt->second;
    sptr<graphics::Node> parent = parentIt->second;

    auto topIt = std::find(topNodes.begin(), topNodes.end(), child);
    if (topIt != topNodes.end()) topNodes.erase(topIt);

    child->parent = parent;
    parent->children.push_back(child);
}

void GltfScene::removeNode(const str& name) {
    auto it = nodes.find(name);
    if (it == nodes.end()) return;

    sptr<graphics::Node> node = it->second;

    auto topIt = std::find(topNodes.begin(), topNodes.end(), node);
    if (topIt != topNodes.end()) topNodes.erase(topIt);

    if (auto parent = node->parent.lock()) {
        auto& siblings = parent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
    }

    nodes.erase(it);
}

sptr<graphics::Node> GltfScene::getNode(const str& name) const {
    auto it = nodes.find(name);
    return it != nodes.end() ? it->second : nullptr;
}

void GltfScene::setDefaultMaterial(const graphics::MaterialInstance& material) {
    defaultMaterial = material;
    hasDefaultMaterial = true;
}

void GltfScene::drawImGui() {
    if (ImGui::Begin("Scene Info")) {
        ImGui::Text("Nodes: %zu", nodes.size());
        ImGui::Text("Opaque surfaces: %zu", drawContext.opaqueSurfaces.size());
        ImGui::Text("Transparent surfaces: %zu", drawContext.transparentSurfaces.size());

        if (renderingTechnique) {
            ImGui::Text("Technique: %s", renderingTechnique->getName().c_str());
        }

        if (renderingTechnique && renderingTechnique->getTechnique() == graphics::techniques::TechniqueType::ShadowMapping) {
            auto* shadow = static_cast<graphics::techniques::ShadowMappingTechnique*>(renderingTechnique);
            bool displayShadowMap = shadow->isDisplayingShadowMap();
            if (ImGui::Checkbox("Display Shadow Map", &displayShadowMap)) {
                shadow->setDisplayShadowMap(displayShadowMap);
            }
            bool enablePCF = shadow->isPCFEnabled();
            if (ImGui::Checkbox("Enable PCF", &enablePCF)) {
                shadow->setEnablePCF(enablePCF);
            }
        }

        if (renderingTechnique && renderingTechnique->getTechnique() == graphics::techniques::TechniqueType::Deferred) {
            auto* deferred = static_cast<graphics::techniques::DeferredRenderingTechnique*>(renderingTechnique);
            const char* debugModes[] = { "None", "Position", "Normal", "Albedo", "Depth" };
            int currentMode = static_cast<int>(deferred->getDebugMode());
            if (ImGui::Combo("Debug Mode", &currentMode, debugModes, IM_ARRAYSIZE(debugModes))) {
                deferred->setDebugMode(static_cast<graphics::techniques::DeferredRenderingTechnique::DebugMode>(currentMode));
            }
        }

        ImGui::Separator();
        ImGui::Text("Post-Processing");
        auto& bloomParams = renderer->getBloomParams();
        ImGui::Checkbox("Enable Bloom", &bloomParams.enabled);
        if (bloomParams.enabled) {
            ImGui::SliderFloat("Threshold", &bloomParams.threshold, 0.0f, 2.0f);
            ImGui::SliderFloat("Intensity", &bloomParams.intensity, 0.0f, 5.0f);
            ImGui::SliderFloat("Blur Scale", &bloomParams.blurScale, 0.1f, 3.0f);
            ImGui::SliderFloat("Blur Strength", &bloomParams.blurStrength, 0.5f, 3.0f);
            ImGui::SliderFloat("Bloom Strength", &bloomParams.bloomStrength, 0.0f, 2.0f);
            ImGui::SliderFloat("Exposure", &bloomParams.exposure, 0.1f, 5.0f);
        }

        if (renderingTechnique && (renderingTechnique->getTechnique() == graphics::techniques::TechniqueType::Deferred ||
                                   renderingTechnique->getTechnique() == graphics::techniques::TechniqueType::ShadowMapping)) {
            ImGui::Separator();
            auto& ssaoParams = renderer->getSSAOParams();
            ImGui::Checkbox("Enable SSAO", &ssaoParams.enabled);
            if (ssaoParams.enabled) {
                ImGui::SliderFloat("SSAO Radius", &ssaoParams.radius, 0.1f, 50.0f);
                ImGui::SliderFloat("SSAO Bias", &ssaoParams.bias, 0.001f, 1.0f);
                ImGui::SliderFloat("SSAO Intensity", &ssaoParams.intensity, 0.5f, 5.0f);
                ImGui::Checkbox("SSAO Blur", &ssaoParams.blurEnabled);
                ImGui::Checkbox("SSAO Only (Debug)", &ssaoParams.ssaoOnly);
            }
        }
    }
    ImGui::End();
}
