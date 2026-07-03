#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Per-frame constants shared by every landscape shader, uploaded once per
// frame to UBO binding 0. std140: keep every member vec4/mat4-sized so the
// C++ layout matches GLSL with no padding surprises. Grows brick by brick
// (sun, ambient, fog, cascades).
struct FrameUniforms {
    Mat4 viewProj {};
    Vec4 cameraPos {};   // xyz = world position, w unused
    Vec4 time {};        // x = seconds since scene start, yzw unused
};

} // namespace render
