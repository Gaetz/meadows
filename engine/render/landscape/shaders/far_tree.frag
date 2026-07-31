#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "clouds.glsl"
#include "view_util.glsl"

// Far-tree impostor silhouette: an ANALYTIC stylized tree in the quad —
// a few hash-jittered crown discs over a thin trunk (the lobe-tree
// family's read at distance) — no texture bake, cel-friendly, and a few
// pixels tall where it shows. Lit like the far terrain (ambient + sun ×
// cloud shadow), dissolved by applyFog.
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vParams;
layout(location = 3) in float vFade;
layout(location = 0) out vec4 fragColor;

float hash1(float n) {
    return fract(sin(n) * 43758.5453123);
}

void main() {
    // Dither dissolve for the distance fades (IGN — keeps depth writes).
    float ign = ignJitter(gl_FragCoord.xy);
    if (vFade <= ign) {
        discard;
    }
    // Crown: discs jittered by the instance seed, filling the band
    // above the measured trunk fraction; trunk: a thin stem under
    // them. Everything in quad space (x -0.5..0.5, y 0..1).
    float s = vParams.x;
    float trunkFrac = vParams.w;
    float crownMid = trunkFrac + (1.0 - trunkFrac) * 0.55;
    float crownHalf = (1.0 - trunkFrac) * 0.55;
    float inside = 0.0;
    for (int i = 0; i < 4; ++i) {
        float fi = float(i);
        vec2 center =
            vec2((hash1(s + fi * 7.31) - 0.5) * 0.42,
                 crownMid + (hash1(s + fi * 3.77) - 0.5) * crownHalf);
        float radius =
            (0.26 + hash1(s + fi * 11.9) * 0.16) * (1.0 - trunkFrac) / 0.6;
        vec2 q = vUv - center;
        q.y *= 1.1; // slightly flattened lobes
        inside = max(inside, step(length(q), radius));
    }
    float trunk = step(abs(vUv.x), 0.03) *
                  step(vUv.y, trunkFrac + 0.08);
    if (max(inside, trunk) < 0.5) {
        discard;
    }
    float cloudVis = cloudShadowFactor(vWorldPos);
    vec3 base = mix(vec3(0.13, 0.22, 0.10), vec3(0.20, 0.30, 0.13),
                    vParams.y);
    vec3 tint = mix(vec3(0.23, 0.16, 0.11), base, inside); // trunk brown
    // Flat lighting: no normal on a silhouette — ambient + a slice of
    // sun that follows the cloud shadows so the far woods flicker with
    // the passing clouds like everything else.
    vec3 lit = tint * (uAmbientColor.rgb + uSunColor.rgb * (0.55 * cloudVis));
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
}
