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

        // descriptorPool cleans itself up in its destructor or needs manual cleanup if allocated
        // But DescriptorAllocatorGrowable desctructor handles pool destruction usually
        // Let's check if we need to manually destroy. The user said check destructors. 
        // DescriptorAllocatorGrowable dtor destroys pools.
        
        // materialDataBuffer dtor might NOT destroy the buffer handle if it's only RAII on the wrapper, 
        // but looking at Buffer.h, it has a destroy() method. 
        materialDataBuffer.destroy();

        for (auto& [name, image] : images) {
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
