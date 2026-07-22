#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// CPU mesh data, shared by the asset loaders (GltfMesh) and the renderer's
// mesh builders. Home is engine/assets/ so the loaders never include
// engine/render/ (same dependency direction as HeightPatches). The
// namespace stays `render` to avoid churning every consumer; these are plain
// CPU structs with no GPU dependency.

namespace render {

// One interleaved CPU vertex format shared by every landscape mesh (terrain,
// trees, props): a single layout keeps pipelines and MeshBuilder simple.
// Matches shader locations 0..3.
struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec3 color;
};

// CPU-side mesh, built on any thread, uploaded on the main thread.
struct MeshData {
    vector<MeshVertex> vertices;
    vector<u32> indices;
};

// Skinned variant: the static layout + joint influences.
// Joints ride as floats because the RHI's vertex formats are float-only —
// exact up to 2^24 joints, i.e. forever. Matches skinned.vert locations
// 0..5; the bone palette itself is an SSBO, not a vertex stream.
struct SkinnedVertex {
    Vec3 position; // bind-pose mesh space (node transforms don't apply to
    Vec3 normal;   //   skinned vertices — the palette places them)
    Vec2 uv;
    Vec3 color;
    Vec4 joints;  // 4 palette indices, float-encoded
    Vec4 weights; // normalized influence weights
};

struct SkinnedMeshData {
    vector<SkinnedVertex> vertices;
    vector<u32> indices;
};

} // namespace render
