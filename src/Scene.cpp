//
// Created by dsin on 11/01/2026.
//

#include "Scene.h"
#include "Graphics/Renderer.h"
#include "Graphics/RenderObject.h"
#include "Graphics/Pipelines/GLTFMetallicRoughness.h"
#include "Graphics/Techniques/IRenderingTechnique.h"
#include <imgui.h>

Scene::Scene(graphics::Renderer* renderer)
    : renderer(renderer)
{
    // Initialize descriptor pool for scene materials
    vector<graphics::DescriptorAllocatorGrowable::PoolSizeRatio> sizes = {
        { vk::DescriptorType::eCombinedImageSampler, 3 },
        { vk::DescriptorType::eUniformBuffer, 3 },
    };

    vk::Device device = renderer->getContext()->getDevice();
    descriptorPool = graphics::DescriptorAllocatorGrowable(device, 100, sizes);

    // Initialize default
    initializeDefaultMaterial();
    renderer->setAnimateLight(false);
}

Scene::~Scene() {
    nodes.clear();
    topNodes.clear();
    materialConstantsBuffer.destroy();
}

void Scene::initializeDefaultMaterial() {
    using namespace graphics::pipelines;

    vk::Device device = renderer->getContext()->getDevice();

    // Create material constants buffer
    materialConstantsBuffer = graphics::Buffer(
        renderer->getContext(),
        sizeof(GLTFMetallicRoughness::MaterialConstants),
        vk::BufferUsageFlagBits::eUniformBuffer,
        VMA_MEMORY_USAGE_CPU_TO_GPU
    );

    // Write default constants (white, slightly rough non-metal)
    auto* constants = static_cast<GLTFMetallicRoughness::MaterialConstants*>(
        materialConstantsBuffer.info.pMappedData
    );
    constants->colorFactors = Vec4(1.0f, 1.0f, 1.0f, 1.0f);
    constants->metalRoughFactors = Vec4(0.0f, 0.5f, 0.0f, 0.0f);

    // Create material resources using renderer's default textures
    GLTFMetallicRoughness::MaterialResources resources;
    resources.colorImage = renderer->whiteImage;
    resources.colorSampler = renderer->defaultSamplerLinear;
    resources.metalRoughImage = renderer->whiteImage;
    resources.metalRoughSampler = renderer->defaultSamplerLinear;
    resources.dataBuffer = materialConstantsBuffer.buffer;
    resources.dataBufferOffset = 0;

    // Create material instance via Renderer's material factory
    defaultMaterial = renderer->metalRoughMaterial.writeMaterial(
        device,
        graphics::MaterialPass::MainColor,
        resources,
        &descriptorPool
    );

    hasDefaultMaterial = true;
}

void Scene::update() {
    // Clear draw context
    drawContext.opaqueSurfaces.clear();
    drawContext.transparentSurfaces.clear();

    // Refresh transforms for all top-level nodes
    for (auto& node : topNodes) {
        node->refreshTransform(Mat4(1.0f));
    }

    // Fill draw context with all renderable nodes
    for (auto& node : topNodes) {
        node->draw(Mat4(1.0f), drawContext);
    }
}

sptr<graphics::Node> Scene::addNode(const str& name) {
    auto node = std::make_shared<graphics::Node>();
    node->localTransform = Mat4(1.0f);
    node->worldTransform = Mat4(1.0f);

    nodes[name] = node;
    topNodes.push_back(node);

    return node;
}

sptr<graphics::MeshNode> Scene::addMeshNode(const str& name, sptr<graphics::MeshAsset> mesh) {
    auto meshNode = std::make_shared<graphics::MeshNode>();
    meshNode->mesh = mesh;
    meshNode->localTransform = Mat4(1.0f);
    meshNode->worldTransform = Mat4(1.0f);

    // If surfaces don't have materials, assign the default
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

void Scene::setParent(const str& childName, const str& parentName) {
    auto childIt = nodes.find(childName);
    auto parentIt = nodes.find(parentName);

    if (childIt == nodes.end() || parentIt == nodes.end()) {
        return;
    }

    sptr<graphics::Node> child = childIt->second;
    sptr<graphics::Node> parent = parentIt->second;

    // Remove from topNodes if it was a root
    auto topIt = std::find(topNodes.begin(), topNodes.end(), child);
    if (topIt != topNodes.end()) {
        topNodes.erase(topIt);
    }

    // Set up parent-child relationship
    child->parent = parent;
    parent->children.push_back(child);
}

void Scene::removeNode(const str& name) {
    auto it = nodes.find(name);
    if (it == nodes.end()) {
        return;
    }

    sptr<graphics::Node> node = it->second;

    // Remove from topNodes if present
    auto topIt = std::find(topNodes.begin(), topNodes.end(), node);
    if (topIt != topNodes.end()) {
        topNodes.erase(topIt);
    }

    // Remove from parent's children if it has a parent
    if (auto parent = node->parent.lock()) {
        auto& siblings = parent->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), node), siblings.end());
    }

    // Remove from nodes map
    nodes.erase(it);
}

sptr<graphics::Node> Scene::getNode(const str& name) const {
    auto it = nodes.find(name);
    if (it != nodes.end()) {
        return it->second;
    }
    return nullptr;
}

void Scene::setDefaultMaterial(const graphics::MaterialInstance& material) {
    defaultMaterial = material;
    hasDefaultMaterial = true;
}

void Scene::drawImGui() {
    // Default ImGui for base Scene - can be overridden by derived classes
    if (ImGui::Begin("Scene Info")) {
        ImGui::Text("Nodes: %zu", nodes.size());
        ImGui::Text("Opaque surfaces: %zu", drawContext.opaqueSurfaces.size());
        ImGui::Text("Transparent surfaces: %zu", drawContext.transparentSurfaces.size());

        if (renderingTechnique) {
            ImGui::Text("Technique: %s", renderingTechnique->getName());
        }
    }
    ImGui::End();
}
