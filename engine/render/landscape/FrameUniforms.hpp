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
    Vec4 ambientColor {};  // rgb = sky ambient term, w = stylized-lighting
                           // blend (0 classic wrap, 1 BotW step ramp)
    Vec4 zenithColor {};      // rgb = sky gradient top
    Vec4 horizonColor {};     // rgb = horizon on the SUN side (warm at dusk)
    Vec4 horizonFarColor {};  // rgb = horizon opposite the sun (already night)
    Vec4 terrainInfo {};      // x = sea level, y = snow line (m),
                              // z = splat UV scale (tiles/meter), w unused
    Vec4 postInfo {};         // x = filmic tonemap (0/1), y = exposure
    Vec4 fogInfo {};          // x = density, y = height falloff (1/m),
                              // z = low-altitude boost, w = start distance (m)
    array<Mat4, 3> sunViewProj {}; // world -> cascade shadow clip space
    Vec4 cascadeSplits {};    // xyz = cascade far view-distances (m)
    Vec4 shadowInfo {};       // xyz = world texel size per cascade,
                              // w = shadow strength (0 = shadows off)
    Vec4 screenInfo {};       // xy = viewport size (px), zw = 1/size
    Vec4 cloudInfo {};        // x = coverage [0,1], y = layer height (m),
                              // z = pattern scale (1/m), w = shadow strength
    Vec4 sunScreen {};        // xy = sun position in screen UV, z = shaft
                              // visibility fade, w = god-ray intensity
    Vec4 cloudMapInfo {};     // xy = baked cloud-field center (world XZ),
                              // z = 1/span, w unused
    Vec4 waterMapInfo {};     // xy = pool-depth map center (world XZ),
                              // z = 1/span, w unused
    Vec4 windInfo {};         // x = accumulated wind time (s, speed-scaled —
                              // keeps cloud/wave phase continuous when the
                              // weather changes the wind), y = sway strength,
                              // z = water chop multiplier, w unused
    // Brick 33b/c (APPENDED — the UBO lesson): the terrain light map.
    Vec4 terrainLightInfo {}; // xy = map center (world XZ), z = 1/span,
                              // w = strength (0 = feature off)
    // Brick 32 (APPENDED): x = the EFFECTIVE water surface Y above the
    // camera (sea level outdoors, a volume's top when inside one,
    // -1e6 = dry) — the tonemap submersion reads this, not seaLevel.
    Vec4 submersionInfo { -1.0e6f, 0.0f, 0.0f, 0.0f };
    // B2b (APPENDED): the interior key-light shadow — world -> light clip
    // for the ONE castsShadow light nearest the camera.
    Mat4 keyShadowViewProj {};
    Vec4 keyShadowInfo {}; // xyz = that light's position, w = active
    // Brick 30/31 (APPENDED): x = storm front 0-1 (horizon cumulonimbus),
    // y = rain intensity 0-1 (streaks + wetness).
    Vec4 stormInfo {};
    // Brick 31 (APPENDED): world -> top-down ortho clip for the rain
    // occlusion depth (no rain under roofs).
    Mat4 rainOcclusionViewProj {};
    // 7.8ter (APPENDED): interactive grass bending — xy = the player's
    // feet XZ, z = feet Y, w = bend radius (0 = off, e.g. Fly mode).
    Vec4 grassBendInfo {};
};

} // namespace render
