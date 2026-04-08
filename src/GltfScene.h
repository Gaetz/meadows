#pragma once
#include "IScene.h"
#include "Defines.h"
#include "Graphics/Types.h"
#include "Graphics/Node.h"
#include "Graphics/VulkanLoader.h"
#include "Graphics/DescriptorAllocatorGrowable.h"
#include "Graphics/Buffer.h"
#include "Graphics/Image.h"
#include "Graphics/RenderObject.h"
#include <optional>

namespace graphics {
    class Renderer;
}

namespace graphics::techniques {
    class IRenderingTechnique;
}

class GltfScene : public IScene {
public:
    explicit GltfScene(graphics::Renderer* renderer,
                       const str& gltfPath = "",
                       Mat4 modelTransform = Mat4{1.f});
    ~GltfScene() override;

    // IScene interface
    void update() override;
    void drawImGui() override;
    graphics::DrawContext& getDrawContext() override { return drawContext; }
    graphics::techniques::IRenderingTechnique* getRenderingTechnique() const override { return renderingTechnique; }
    void setRenderingTechnique(graphics::techniques::IRenderingTechnique* technique) override;
    void onActivated(graphics::Renderer* renderer) override;

    // Load and apply a KTX color map to all model surfaces (e.g. armor model)
    void loadKTXColorMap(const str& colorKtxPath);

    // Camera configuration applied on scene activation
    Vec3  initialCameraPos   { 0.f, 0.f, 5.f };
    float initialCameraPitch { 0.f };
    float initialCameraYaw   { 0.f };
    bool  animateLight       { true };

    // Node management (for manually assembled scenes)
    sptr<graphics::Node>     addNode(const str& name);
    sptr<graphics::MeshNode> addMeshNode(const str& name, sptr<graphics::MeshAsset> mesh);
    void setParent(const str& childName, const str& parentName);
    void removeNode(const str& name);
    sptr<graphics::Node>     getNode(const str& name) const;

    // Material management
    void setDefaultMaterial(const graphics::MaterialInstance& material);
    graphics::MaterialInstance* getDefaultMaterial() { return &defaultMaterial; }

    const std::unordered_map<str, sptr<graphics::Node>>& getNodes() const { return nodes; }
    const vector<sptr<graphics::Node>>& getTopNodes() const { return topNodes; }

protected:
    graphics::Renderer* renderer;
    graphics::techniques::IRenderingTechnique* renderingTechnique { nullptr };

    // Optional glTF model (null if scene is assembled manually via nodes)
    sptr<graphics::LoadedGLTF> model;
    Mat4 modelTransform;

    // Node hierarchy for manually assembled scenes
    std::unordered_map<str, sptr<graphics::Node>> nodes;
    vector<sptr<graphics::Node>> topNodes;

    // Draw context filled during update()
    graphics::DrawContext drawContext;

    // Default material
    graphics::MaterialInstance defaultMaterial;
    graphics::Buffer materialConstantsBuffer;
    graphics::DescriptorAllocatorGrowable descriptorPool;
    bool hasDefaultMaterial { false };

    // Optional KTX resources owned by this scene
    std::optional<graphics::Image> ktxColorMap;
    graphics::Buffer ktxMaterialBuffer;

    void initializeDefaultMaterial();
};
