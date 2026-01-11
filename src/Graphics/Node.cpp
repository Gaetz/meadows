//
// Created by dsin on 09/01/2026.
//

#include "Node.h"

#include "VulkanLoader.h"

namespace graphics {
    void Node::refreshTransform(const Mat4& parentMatrix)
    {
        worldTransform = parentMatrix * localTransform;
        for (auto c : children) {
            c->refreshTransform(worldTransform);
        }
    }

    void Node::draw(const Mat4& topMatrix, DrawContext& ctx) {
        // Draw children
        for (const auto& c : children) {
            c->draw(topMatrix, ctx);
        }
    }

    void MeshNode::draw(const Mat4 &topMatrix, DrawContext &ctx) {
        const Mat4 nodeMatrix = topMatrix * worldTransform;

        // A mesh can have multiple surfaces with different materials, so we will loop the surfaces
        // of the mesh, and add the resulting RenderObjects to the list.
        for (auto&[startIndex, count, material] : mesh->surfaces) {
            RenderObject def;
            def.indexCount = count;
            def.firstIndex = startIndex;
            def.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
            def.material = &material->data;

            def.transform = nodeMatrix;
            def.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;

            if (material->data.passType == MaterialPass::Transparent) {
                ctx.transparentSurfaces.push_back(def);
            } else {
                ctx.opaqueSurfaces.push_back(def);
            }
        }

        // Recurse down
        Node::draw(topMatrix, ctx);
    }
}
