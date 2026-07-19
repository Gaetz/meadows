#version 460 core
#include "common.glsl"

// Brick 33a — screen-space contact shadows (the Bend Studio GDC
// technique, reimplemented): from each pixel, march a short distance
// TOWARD the sun in world space; if the depth buffer holds geometry in
// front of a marched probe (within a thickness window, against haloing),
// the pixel sits in a contact shadow the 2048² CSM cannot resolve —
// grass blades, prop feet, NPC soles. Half-res, composited by the
// tonemap as a multiplier (the SSAO pattern). The scene skips this pass
// and clears the target to WHITE when the toggle is off (no free
// FrameUbo slot for a flag).
layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec3 worldFromDepth(vec2 uv, float depth) {
    // 0..1 clip: the stored depth IS ndc z (no *2-1 remap).
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    // No sun (interiors, night): neutral.
    if (uSunColor.r + uSunColor.g + uSunColor.b <= 0.001 ||
        uSunDirection.y <= 0.0) {
        fragColor = vec4(1.0);
        return;
    }
    float depth = texture(uSceneDepth, vUv).r;
    if (depth >= 0.99995) {
        fragColor = vec4(1.0); // sky
        return;
    }
    vec3 position = worldFromDepth(vUv, depth);
    float dist = distance(position, uCameraPos.xyz);

    // Reach grows a little with distance so the effect keeps a similar
    // on-screen footprint; thickness bounds what counts as an occluder.
    float reach = 0.45 + dist * 0.01;
    float thickness = 0.35 + dist * 0.01;

    const int kSteps = 12;
    // IGN jitter breaks the marching bands into filterable noise.
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                            0.00583715 * gl_FragCoord.y));
    float stepLen = reach / float(kSteps);
    float shadow = 0.0;
    for (int i = 1; i <= kSteps; ++i) {
        vec3 p = position +
                 uSunDirection.xyz * ((float(i) - 0.5 + jitter) * stepLen);
        vec4 clip = uViewProj * vec4(p, 1.0);
        if (clip.w <= 0.0) {
            break;
        }
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) ||
            any(greaterThan(uv, vec2(1.0)))) {
            break;
        }
        vec3 surface = worldFromDepth(uv, texture(uSceneDepth, uv).r);
        float surfaceDist = distance(surface, uCameraPos.xyz);
        float probeDist = distance(p, uCameraPos.xyz);
        float ahead = probeDist - surfaceDist; // >0: geometry in front
        // Distance-scaled floor (speckle fix, 2026-07-10): at range the
        // depth reconstruction error alone exceeds a fixed 0.02 m and
        // surfaces self-shadowed into black dots — worst at altitude
        // where everything is far.
        float minAhead = 0.02 + dist * 0.0025;
        if (ahead > minAhead && ahead < thickness) {
            shadow = 1.0;
            break;
        }
    }
    // Soft floor: contact shadows darken, never black out (the CSM and
    // ambient own the real shadow terms).
    float lit = 1.0 - shadow * 0.45;

    // Dev ask 2026-07-11: contact and CSM combine as a MAX, not a
    // product — the tonemap multiplies our output over a color that
    // already carries the sun shadow, so we emit the RATIO
    // min(1, contact/sun): full CSM shadow -> 1 (contact adds nothing),
    // full sun -> the raw contact term. Offset toward the sun replaces
    // the normal bias (we have no normals here).
    float sun = shadowFactor(position + uSunDirection.xyz * 0.3,
                             uSunDirection.xyz);
    fragColor = vec4(vec3(sun < 0.05 ? 1.0 : min(1.0, lit / sun)), 1.0);
}
