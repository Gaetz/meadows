#version 460 core
#include "common.glsl"

// Full-res SSDM resolve (see ssdm_resolve_body.glsl): rewrites the
// offscreen target in place.
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uSceneDepth;
layout(binding = 2) uniform sampler2D uFlow;
layout(binding = 3) uniform sampler2D uBounds0;
layout(binding = 4) uniform sampler2D uBounds1;
layout(binding = 5) uniform sampler2D uBounds2;
layout(binding = 6) uniform sampler2D uBounds3;
layout(binding = 7) uniform sampler2D uBounds4;
#include "view_util.glsl"
#include "ssdm_common.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

#include "ssdm_resolve_body.glsl"
