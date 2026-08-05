#version 460 core
#include "common.glsl"

// SSDM scatter, pass 3/3 — the RESOLVE (Lobel 2008): every destination
// pixel walks the bounds quadtree top-down, visiting only the nodes
// whose displaced bbox can cover it, and keeps the NEAREST candidate
// (largest reversed-Z displaced depth) — crests genuinely EXTRUDE over
// their neighbors, sky included, and occlude instead of stretching.
// Pixels nobody lands on (dis-occluded pits) fall back to the v1
// gather warp — pits keep digging, and the two directions of relief
// finally coexist.
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

vec4 boundsAt(int level, ivec2 t) {
    switch (level) {
    case 4: return texelFetch(uBounds4, t, 0);
    case 3: return texelFetch(uBounds3, t, 0);
    case 2: return texelFetch(uBounds2, t, 0);
    case 1: return texelFetch(uBounds1, t, 0);
    }
    return texelFetch(uBounds0, t, 0);
}

void main() {
    ivec2 pPix = ivec2(gl_FragCoord.xy);
    vec2 pC = vec2(pPix) + 0.5;
    ivec2 res = textureSize(uSceneDepth, 0);

    float bestDepth = -1.0;
    ivec2 bestLeaf = ivec2(-1);
    // Seed: the level-4 (16 px) tiles whose footprint can reach p
    // (displacement is bounded to kSsdmMaxPx).
    const int kReach = int(kSsdmMaxPx) + 1;
    ivec2 lo = (pPix - kReach) >> 4;
    ivec2 hi = (pPix + kReach) >> 4;
    ivec3 stack[64];
    int sp = 0;
    for (int ty = lo.y; ty <= hi.y; ++ty) {
        for (int tx = lo.x; tx <= hi.x; ++tx) {
            if (tx >= 0 && ty >= 0) {
                stack[sp++] = ivec3(tx, ty, 4);
            }
        }
    }
    for (int iter = 0; iter < 160 && sp > 0; ++iter) {
        ivec3 node = stack[--sp];
        int level = node.z;
        ivec2 levelSize = max(res >> level, ivec2(1));
        if (node.x >= levelSize.x || node.y >= levelSize.y) {
            continue;
        }
        vec4 b = boundsAt(level, node.xy);
        if (pC.x < b.x || pC.y < b.y || pC.x > b.z || pC.y > b.w) {
            continue; // no child can land here
        }
        if (level == 0) {
            vec4 flow = texelFetch(uFlow, node.xy, 0);
            vec2 landed = vec2(node.xy) + 0.5 + flow.rg;
            if (max(abs(landed.x - pC.x), abs(landed.y - pC.y)) <=
                    0.8 &&
                flow.b > bestDepth) {
                bestDepth = flow.b;
                bestLeaf = node.xy;
            }
        } else if (sp <= 59) {
            ivec2 c = node.xy * 2;
            int child = level - 1;
            stack[sp++] = ivec3(c, child);
            stack[sp++] = ivec3(c + ivec2(1, 0), child);
            stack[sp++] = ivec3(c + ivec2(0, 1), child);
            stack[sp++] = ivec3(c + ivec2(1, 1), child);
        }
    }
    if (bestLeaf.x >= 0) {
        fragColor = texelFetch(uSceneColor, bestLeaf, 0);
        return;
    }
    // Hole (everything moved away): the v1 gather digs the pit.
    vec2 texel = 1.0 / vec2(res);
    vec2 q = vUv;
    float unusedDepth;
    for (int i = 0; i < 3; ++i) {
        q = vUv - ssdmDelta(q, texel, unusedDepth);
    }
    fragColor = texture(uSceneColor, q);
}
