#pragma once

#include <cstddef> // offsetof

#include "engine/assets/MeshData.hpp"
#include "engine/rhi/Rhi.hpp"

namespace render {

// The MeshVertex vertex-buffer layout, written ONCE (audit U3-5): every
// lit pipeline streaming MeshVertex (terrain, vegetation, scene meshes)
// binds the same four attributes. A MeshVertex field change now updates
// every pipeline instead of five hand-copied descriptor blocks.
inline rhi::VertexBufferLayout meshVertexLayout() {
    return { .stride = sizeof(MeshVertex),
             .attributes = { { .location = 0,
                               .format = rhi::VertexFormat::F32x3,
                               .offset = offsetof(MeshVertex, position) },
                             { .location = 1,
                               .format = rhi::VertexFormat::F32x3,
                               .offset = offsetof(MeshVertex, normal) },
                             { .location = 2,
                               .format = rhi::VertexFormat::F32x2,
                               .offset = offsetof(MeshVertex, uv) },
                             { .location = 3,
                               .format = rhi::VertexFormat::F32x3,
                               .offset = offsetof(MeshVertex, color) } } };
}

// Depth-only variant (shadow casters): position alone over the FULL
// MeshVertex stride — the same buffers as the lit pass.
inline rhi::VertexBufferLayout meshVertexPositionLayout() {
    return { .stride = sizeof(MeshVertex),
             .attributes = { { .location = 0,
                               .format = rhi::VertexFormat::F32x3,
                               .offset = offsetof(MeshVertex, position) } } };
}

} // namespace render
