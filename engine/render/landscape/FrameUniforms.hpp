#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Per-frame constants shared by every landscape shader, uploaded once per
// frame to UBO binding 0. std140: keep every member vec4/mat4-sized so the
// C++ layout matches GLSL with no padding surprises. Must match the FrameUbo
// block in shaders/common.glsl field for field.
struct FrameUniforms {
    Mat4 viewProj {};
    Mat4 invViewProj {};   // NDC -> world, for fullscreen ray reconstruction
    Vec4 cameraPos {};     // xyz = world position, w unused
    Vec4 time {};          // x = seconds since scene start, yzw unused
    Vec4 sunDirection {};  // xyz = normalized, points TOWARD the sun
    Vec4 sunColor {};      // rgb = sun light color, w = sun disc intensity
    Vec4 sunGlowColor {};  // rgb = sky halo/afterglow (outlives the disc)
    Vec4 ambientColor {};  // rgb = sky ambient term
    Vec4 zenithColor {};      // rgb = sky gradient top
    Vec4 horizonColor {};     // rgb = horizon on the SUN side (warm at dusk)
    Vec4 horizonFarColor {};  // rgb = horizon opposite the sun (already night)
    Vec4 terrainInfo {};      // x = sea level, y = snow line (m),
                              // z = splat UV scale (tiles/meter), w unused
};

} // namespace render
