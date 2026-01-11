#pragma once
#include "Types.h"

namespace graphics {
    struct MeshAsset;

    struct DrawContext;

    // Base class for a renderable dynamic object
    class IRenderable {
    public:
        virtual ~IRenderable() = default;

    private:
        virtual void draw(const Mat4& topMatrix, DrawContext& ctx) = 0;
    };

    // Implementation of a drawable scene node.
    // The scene node can hold children and will also keep a transform to propagate to them
    struct Node : public IRenderable {
        // Parent pointer must be a weak pointer to avoid circular dependencies
        std::weak_ptr<Node> parent;
        std::vector<sptr<Node>> children;

        Mat4 localTransform;
        Mat4 worldTransform;

        void refreshTransform(const Mat4& parentMatrix);
        void draw(const Mat4& topMatrix, DrawContext& ctx) override;
    };

    struct MeshNode : public Node {
        sptr<MeshAsset> mesh;
        void draw(const Mat4& topMatrix, DrawContext& ctx) override;
    };

}

