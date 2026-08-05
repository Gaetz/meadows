#version 460 core
#include "common.glsl"

// SSDM scatter, pass 1/3 — the FLOW: per source pixel, its displacement
// in PIXELS (rg, signed) and the displaced reversed-Z depth (b) — the
// resolve's nearest-wins key. Flat/neutral/sky pixels emit zero delta
// with their own depth, so they cover themselves.
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uSceneDepth;
#include "view_util.glsl"
#include "ssdm_common.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 resolution = vec2(textureSize(uSceneDepth, 0));
    vec2 texel = 1.0 / resolution;
    float dispDepth;
    vec2 delta = ssdmDelta(vUv, texel, dispDepth);
    fragColor = vec4(delta * resolution, dispDepth, 0.0);
}
