#version 460 core
#include "common.glsl"

// SSDM scatter, pass 2/3 level 0 — bounding boxes: each texel = the
// displaced ABSOLUTE position of its source pixel (min = max), padded
// by the coverage radius. The downsample chain unions 2x2 into the
// quadtree the resolve prunes with.
layout(binding = 0) uniform sampler2D uFlow;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 flow = texelFetch(uFlow, p, 0);
    vec2 u = vec2(p) + 0.5 + flow.rg;
    fragColor = vec4(u - 0.8, u + 0.8); // min.xy, max.xy (pixels)
}
