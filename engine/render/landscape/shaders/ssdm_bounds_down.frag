#version 460 core
#include "common.glsl"

// SSDM scatter — bounds pyramid downsample: union of the 2x2 children
// bboxes (min.xy, max.xy in pixels).
layout(binding = 0) uniform sampler2D uPrev;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) * 2;
    ivec2 hi = textureSize(uPrev, 0) - 1;
    vec4 b0 = texelFetch(uPrev, min(p, hi), 0);
    vec4 b1 = texelFetch(uPrev, min(p + ivec2(1, 0), hi), 0);
    vec4 b2 = texelFetch(uPrev, min(p + ivec2(0, 1), hi), 0);
    vec4 b3 = texelFetch(uPrev, min(p + ivec2(1, 1), hi), 0);
    fragColor = vec4(min(min(b0.xy, b1.xy), min(b2.xy, b3.xy)),
                     max(max(b0.zw, b1.zw), max(b2.zw, b3.zw)));
}
