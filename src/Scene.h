#pragma once
#include "Defines.h"
#include "Graphics/Types.h"
#include "Graphics/Node.h"
#include "Graphics/VulkanLoader.h"
#include "Graphics/DescriptorAllocatorGrowable.h"
#include "Graphics/Buffer.h"
#include "Graphics/RenderObject.h"

namespace graphics {
    class Renderer;
}

namespace graphics::techniques {
    class IRenderingTechnique;
}

/***
 *
 * Utilisation:
 * // Créer la scène
 * scene = std::make_unique<Scene>(renderer);
 * auto mesh = graphics::loadGltfMeshes(renderer, "assets/cube.glb");
 * scene->addMeshNode("cube", mesh->at(0));
 *
 * // Connecter au renderer
 * renderer->setDrawContext(&scene->getDrawContext());
 *
 * // Dans la boucle principale
 * scene->update();      // Scene remplit son DrawContext
 * renderer->draw();     // Renderer utilise le DrawContext de Scene
**/

class Scene {
public:
    explicit Scene(graphics::Renderer* renderer);
    virtual ~Scene();

    // Core lifecycle methods
    virtual void update();
    virtual void drawImGui();

    // Provides the DrawContext for rendering
    graphics::DrawContext& getDrawContext() { return drawContext; }
    const graphics::DrawContext& getDrawContext() const { return drawContext; }

    // Node management
    sptr<graphics::Node> addNode(const str& name);
    sptr<graphics::MeshNode> addMeshNode(const str& name, sptr<graphics::MeshAsset> mesh);
    void setParent(const str& childName, const str& parentName);
    void removeNode(const str& name);
    sptr<graphics::Node> getNode(const str& name) const;

    // Material management
    void setDefaultMaterial(const graphics::MaterialInstance& material);
    graphics::MaterialInstance* getDefaultMaterial() { return &defaultMaterial; }

    // Rendering technique
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique* technique) { renderingTechnique = technique; }
    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const { return renderingTechnique; }

    // Access to internal structures
    const std::unordered_map<str, sptr<graphics::Node>>& getNodes() const { return nodes; }
    const vector<sptr<graphics::Node>>& getTopNodes() const { return topNodes; }

private:
    graphics::Renderer* renderer;
    graphics::techniques::IRenderingTechnique* renderingTechnique { nullptr };

    // Node hierarchy
    std::unordered_map<str, sptr<graphics::Node>> nodes;
    vector<sptr<graphics::Node>> topNodes;

    // Draw context - filled during update()
    graphics::DrawContext drawContext;

    // Default material system
    graphics::MaterialInstance defaultMaterial;
    graphics::Buffer materialConstantsBuffer;
    graphics::DescriptorAllocatorGrowable descriptorPool;
    bool hasDefaultMaterial { false };

    void initializeDefaultMaterial();
};
