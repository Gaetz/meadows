#version 460 core
#include "common.glsl"

// Screen Space Displacement Mapping prototype (Lobel 2008 — the
// Crimson Desert family), GATHER variant: materials pack a relief
// height into the scene ALPHA (0.5 = flat .. ~0.99 = crest; < 0.5 is
// the grass-exempt flag, >= 0.995 = shaders that never packed —
// neutral). The warp re-fetches the image where the displaced surface
// would land, along the screen-projected surface normal — relief pops
// across interior silhouettes where POM cannot reach. Inverse
// fixed-point iteration: q = p - disp(q); at depth cliffs it stretches
// instead of tearing (the artifact family CD ships with). Known v1
// limit: edges against the SKY do not extrude (a gather has no seed on
// empty pixels — the paper's pyramid scatter is the follow-up if the
// look convinces). uSsaoInfo.z = world amplitude (m); the pass is
// skipped entirely when the toggle is off.
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uSceneDepth;
#include "view_util.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec2 dispAt(vec2 uv, vec2 texel) {
    float a = texture(uSceneColor, uv).a;
    if (a < 0.5 || a >= 0.995) {
        return vec2(0.0); // grass flag / non-participating material
    }
    float d = texture(uSceneDepth, uv).r;
    if (d < 1e-8) {
        return vec2(0.0); // sky
    }
    float h = (a - 0.5) * 2.0 - 0.5; // -0.5 pit .. +0.5 crest
    vec3 w = worldFromDepth(uv, d);
    // Neighbor-difference normal (derivatives are unusable inside the
    // iterated gather). ONE-SIDED per axis, toward the neighbor closest
    // in depth: a two-sided difference crosses the silhouette on thin
    // props (trunks) and the normal blows up right where the warp
    // should shine — the grazing edge.
    float dxp = texture(uSceneDepth, uv + vec2(texel.x, 0.0)).r;
    float dxm = texture(uSceneDepth, uv - vec2(texel.x, 0.0)).r;
    float dyp = texture(uSceneDepth, uv + vec2(0.0, texel.y)).r;
    float dym = texture(uSceneDepth, uv - vec2(0.0, texel.y)).r;
    vec3 dwx =
        abs(dxp - d) <= abs(dxm - d)
            ? worldFromDepth(uv + vec2(texel.x, 0.0), dxp) - w
            : w - worldFromDepth(uv - vec2(texel.x, 0.0), dxm);
    vec3 dwy =
        abs(dyp - d) <= abs(dym - d)
            ? worldFromDepth(uv + vec2(0.0, texel.y), dyp) - w
            : w - worldFromDepth(uv - vec2(0.0, texel.y), dym);
    vec3 n = cross(dwx, dwy);
    float len = length(n);
    if (len < 1e-12) {
        return vec2(0.0);
    }
    n /= len;
    n *= sign(dot(n, uCameraPos.xyz - w));
    vec4 clip = uViewProj * vec4(w + n * (h * uSsaoInfo.z), 1.0);
    if (clip.w <= 0.0) {
        return vec2(0.0);
    }
    vec2 shifted = clip.xy / clip.w * 0.5 + 0.5;
    // Depth-cliff normals go wild: bound the warp in pixels.
    vec2 lim = texel * 10.0;
    return clamp(shifted - uv, -lim, lim);
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSceneColor, 0));
    vec2 q = vUv;
    for (int i = 0; i < 4; ++i) {
        q = vUv - dispAt(q, texel);
    }
    fragColor = texture(uSceneColor, q);
}
