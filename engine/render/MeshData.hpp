#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

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

} // namespace render
