#version 460 core
#include "common.glsl"

// Depth-only pass, except leaf cards: they cut the same holes as
// tree.frag so canopy shadows aren't full rectangles. Same solid ramp
// (there in shadow-texel footprint): distant canopies cast their full
// mass instead of evaporating.
layout(binding = 0) uniform sampler2D uLeafMask;

layout(location = 0) in vec2 vCardUv;

void main() {
    if (vCardUv.x >= 0.0) {
        float alpha = texture(uLeafMask, vCardUv).a;
        float lod = textureQueryLod(uLeafMask, vCardUv).x;
        float solid = smoothstep(uLeafLodInfo.x, uLeafLodInfo.y, lod);
        if (mix(alpha, 1.0, solid) < 0.5) {
            discard;
        }
    }
}
