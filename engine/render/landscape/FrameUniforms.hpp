#pragma once

#include <cstddef> // offsetof (the std140 layout lock below)

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Per-frame constants shared by every landscape shader, uploaded once per
// frame to UBO binding 0. std140: keep every member vec4/mat4-sized so the
// C++ layout matches GLSL with no padding surprises. Must match the FrameUbo
// block in shaders/common.glsl field for field — and new fields are only
// ever APPENDED at the end of the struct (never inserted), so both sides
// stay in sync.
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
    // The terrain light map (far terrain shadows + sky aperture).
    Vec4 terrainLightInfo {}; // xy = map center (world XZ), z = 1/span,
                              // w = strength (0 = feature off)
    // Submersion: x = the EFFECTIVE water surface Y above the
    // camera (sea level outdoors, a volume's top when inside one,
    // -1e6 = dry) — the tonemap submersion reads this, not seaLevel.
    Vec4 submersionInfo { -1.0e6f, 0.0f, 0.0f, 0.0f };
    // The interior key-light shadow — world -> light clip
    // for the ONE castsShadow light nearest the camera.
    Mat4 keyShadowViewProj {};
    Vec4 keyShadowInfo {}; // xyz = that light's position, w = active
    // Weather: x = storm front 0-1 (horizon cumulonimbus),
    // y = rain intensity 0-1 (streaks + wetness).
    Vec4 stormInfo {};
    // World -> top-down ortho clip for the rain
    // occlusion depth (no rain under roofs).
    Mat4 rainOcclusionViewProj {};
    // Interactive grass bending — xy = the player's
    // feet XZ, z = feet Y, w = bend radius (0 = off, e.g. Fly mode).
    Vec4 grassBendInfo {};
    // The meadow's tuning, live
    // from the render panel's "Grass" category (GrassRenderTuning).
    Vec4 grassShapeInfo { 0.95f, 0.045f, 12.5f, 25.0f };
    // x = blade height (m), y = half width (m), z/w = high-detail near/far
    Vec4 grassLodInfo { 10.0f, 70.0f, 0.20f, 1.7f };
    // x/y = density thinning start/end (m), z = far density floor,
    // w = width compensation at far density (wider blades, same mass)
    Vec4 grassBaseColor { 0.012f, 0.040f, 0.008f, 140.0f };
    // rgb = blade base albedo, w = distance fade start (m)
    Vec4 grassTipColor { 0.095f, 0.200f, 0.045f, 190.0f };
    // rgb = blade tip albedo, w = distance fade end (m)
    // The GiTechnique switch — surface shaders
    // swap their ambient term for the merged cascade-0 sample (gi.glsl)
    // when x = 1. Zero = Classic, byte-identical exterior.
    Vec4 giInfo {};     // x = technique (0/1), y = intensity,
                        // z = edge fade width (m), w = grid resolution
    Vec4 giGridInfo {}; // xyz = cascade-0 grid origin, w = probe spacing
    // The fixed log-step GI ramp: x = stops per band
    // (0 = continuous), y = anti-aliasing width across a band edge.
    Vec4 giBandInfo { 0.85f, 0.15f, 0.0f, 0.0f };
};

// --- std140 layout lock (audit U3-3) -------------------------------------------------
// The GLSL FrameUbo block (shaders/common.glsl) mirrors this struct by
// COMMENT only; one member inserted mid-struct (instead of appended) or one
// non-vec4-multiple member silently desyncs every offset after it and
// corrupts ~14 shaders at once (the "UBO lesson"). These asserts pin the
// byte offsets the shaders compile against. To append a member: add it LAST
// in BOTH files, add its offset line here, and bump the size assert. If an
// assert fires on an insertion, you are about to break every existing
// shader read — append instead.
static_assert(offsetof(FrameUniforms, viewProj) == 0);
static_assert(offsetof(FrameUniforms, invViewProj) == 64);
static_assert(offsetof(FrameUniforms, cameraPos) == 128);
static_assert(offsetof(FrameUniforms, time) == 144);
static_assert(offsetof(FrameUniforms, sunDirection) == 160);
static_assert(offsetof(FrameUniforms, sunColor) == 176);
static_assert(offsetof(FrameUniforms, sunGlowColor) == 192);
static_assert(offsetof(FrameUniforms, ambientColor) == 208);
static_assert(offsetof(FrameUniforms, zenithColor) == 224);
static_assert(offsetof(FrameUniforms, horizonColor) == 240);
static_assert(offsetof(FrameUniforms, horizonFarColor) == 256);
static_assert(offsetof(FrameUniforms, terrainInfo) == 272);
static_assert(offsetof(FrameUniforms, postInfo) == 288);
static_assert(offsetof(FrameUniforms, fogInfo) == 304);
static_assert(offsetof(FrameUniforms, sunViewProj) == 320);
static_assert(offsetof(FrameUniforms, cascadeSplits) == 512);
static_assert(offsetof(FrameUniforms, shadowInfo) == 528);
static_assert(offsetof(FrameUniforms, screenInfo) == 544);
static_assert(offsetof(FrameUniforms, cloudInfo) == 560);
static_assert(offsetof(FrameUniforms, sunScreen) == 576);
static_assert(offsetof(FrameUniforms, cloudMapInfo) == 592);
static_assert(offsetof(FrameUniforms, waterMapInfo) == 608);
static_assert(offsetof(FrameUniforms, windInfo) == 624);
static_assert(offsetof(FrameUniforms, terrainLightInfo) == 640);
static_assert(offsetof(FrameUniforms, submersionInfo) == 656);
static_assert(offsetof(FrameUniforms, keyShadowViewProj) == 672);
static_assert(offsetof(FrameUniforms, keyShadowInfo) == 736);
static_assert(offsetof(FrameUniforms, stormInfo) == 752);
static_assert(offsetof(FrameUniforms, rainOcclusionViewProj) == 768);
static_assert(offsetof(FrameUniforms, grassBendInfo) == 832);
static_assert(offsetof(FrameUniforms, grassShapeInfo) == 848);
static_assert(offsetof(FrameUniforms, grassLodInfo) == 864);
static_assert(offsetof(FrameUniforms, grassBaseColor) == 880);
static_assert(offsetof(FrameUniforms, grassTipColor) == 896);
static_assert(offsetof(FrameUniforms, giInfo) == 912);
static_assert(offsetof(FrameUniforms, giGridInfo) == 928);
static_assert(offsetof(FrameUniforms, giBandInfo) == 944);
static_assert(sizeof(FrameUniforms) == 960,
              "FrameUniforms grew: append-only, update common.glsl in "
              "lockstep, then bump this");

} // namespace render
