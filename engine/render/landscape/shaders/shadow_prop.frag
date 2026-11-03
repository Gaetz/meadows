#version 460 core
#include "common.glsl"
#include "view_util.glsl"

// Depth-only pass, except leaf cards: they cut the same holes as
// tree.frag so canopy shadows aren't full rectangles. Same solid ramp
// (there in shadow-texel footprint): distant canopies cast their full
// mass instead of evaporating.
layout(binding = 0) uniform sampler2D uLeafMask;

layout(location = 0) in vec2 vCardUv;
layout(location = 1) in float vPresence;

void main() {
    // The dither handoff in shadow-texel space: the PCF filter turns
    // the dropped texels into a soft shadow fade.
    if (vPresence < 1.0 && vPresence <= ignJitter(gl_FragCoord.xy)) {
        discard;
    }
    if (vCardUv.x >= 0.0) {
        float alpha = texture(uLeafMask, vCardUv).a;
        float lod = textureQueryLod(uLeafMask, vCardUv).x;
        float solid = smoothstep(uLeafLodInfo.x, uLeafLodInfo.y, lod);
        if (mix(alpha, 1.0, solid) < 0.5) {
            discard;
        }
    }
}
