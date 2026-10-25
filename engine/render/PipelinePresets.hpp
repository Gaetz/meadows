#pragma once

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace render {

// The pipeline conventions every landscape pass shares, named once so a
// descriptor reads as intent (docs/RENDERING.md). The scene draws
// REVERSED-Z (near = 1, far = 0, the clear is 0.0): opaque passes test
// Greater and write; transparent/decal passes test Greater without
// writing; the sky is coplanar with the far clear. Shadow casters draw
// plain-Z into their own maps.
inline constexpr rhi::DepthState kDepthOpaque {
    .testEnable = true, .writeEnable = true,
    .compare = rhi::CompareFunc::Greater };
inline constexpr rhi::DepthState kDepthReadOnly {
    .testEnable = true, .writeEnable = false,
    .compare = rhi::CompareFunc::Greater };
inline constexpr rhi::DepthState kDepthBackdrop {
    .testEnable = true, .writeEnable = false,
    .compare = rhi::CompareFunc::GreaterEqual };
inline constexpr rhi::DepthState kDepthCaster {
    .testEnable = true, .writeEnable = true,
    .compare = rhi::CompareFunc::Less };
inline constexpr rhi::DepthState kDepthOff {};

// Polygon offset (hand-tuned): the first line of defense against shadow
// acne on casters, and the toward-camera lift of water sheets lying
// centimetres over the ground.
inline constexpr f32 kDepthBias = 4.0f;
inline constexpr f32 kDepthBiasSlope = 2.5f;

// Uniform-block bindings — the mirror of shaders/common.glsl.
inline const rhi::UniformBlockBinding kFrameUbo { "FrameUbo", 0 };
inline const rhi::UniformBlockBinding kShadowUbo { "ShadowUbo", 1 };
inline const rhi::UniformBlockBinding kRcUbo { "RcUbo", 2 };
inline const rhi::UniformBlockBinding kRcCascadeUbo { "RcCascadeUbo", 4 };
inline const rhi::UniformBlockBinding kLightsUbo { "LightsUbo", 5 };

} // namespace render
