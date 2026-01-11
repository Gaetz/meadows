#include "LoadedGLTF.h"
#include "Renderer.h"

namespace graphics {

    void LoadedGLTF::draw(const Mat4& topMatrix, DrawContext& ctx) {
        // Draw all top-level nodes
        for (auto& n : topNodes) {
            n->draw(topMatrix, ctx);
        }
    }

    void LoadedGLTF::clearAll() {
        if (!creator) return;

        vk::Device device = creator->getContext()->getDevice();
        materialDataBuffer.destroy();

        for (auto& [name, image] : images) {
            if (image.image == creator->errorCheckerboardImage.image) {
                // Don't destroy the default images
                continue;
            }
            image.destroy(creator->getContext());
        }

        for (auto& sampler : samplers) {
            device.destroySampler(sampler);
        }

        for (auto& [name, mesh] : meshes) {
            mesh->meshBuffers.indexBuffer.destroy();
            mesh->meshBuffers.vertexBuffer.destroy();
        }
    }
}
