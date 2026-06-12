#pragma once

#include "Types.h"
#include "Node.h"
#include "VulkanLoader.h"
#include "DescriptorAllocatorGrowable.h"
#include "Buffer.h"
#include "Image.h"

namespace graphics {
    class Renderer;

    /***
     * LoadedGLTF
     *
     * Represents a fully loaded glTF file, with all its meshes, nodes, images, and materials.
     * Implements IRenderable to allow drawing the entire glTF scene graph.
     *
     * The goal with this is that a user would load one GLTF as a level, and that gltf contains
     * all of the textures, meshes, objects needed for that entire level. The user would then also
     * load another GLTF with characters or objects, and keep it loaded for the game.
     */
    class LoadedGLTF : public IRenderable {
    public:
        // Storage for all the data on a given glTF file
        std::unordered_map<str, sptr<MeshAsset>> meshes;
        std::unordered_map<str, sptr<Node>> nodes;
        std::unordered_map<str, Image> images;
        std::unordered_map<str, sptr<GLTFMaterial>> materials;

        // Nodes that dont have a parent, for iterating through the file in tree order
        vector<sptr<Node>> topNodes;

        vector<vk::Sampler> samplers;

        DescriptorAllocatorGrowable descriptorPool;

        Buffer materialDataBuffer;

        Renderer* creator;

        ~LoadedGLTF() override { clearAll(); };

        void draw(const Mat4& topMatrix, DrawContext& ctx) override;

    private:
        void clearAll();
    };
}
