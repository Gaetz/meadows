#version 460 core
#include "common.glsl"
#include "sky.glsl"

// The fog INTEGRATOR (docs/VOLUMETRIC.md V2): half-res, jittered march of
// (inscatter, transmittance) from the fog start to uFogSunInfo.z (the
// composer-set reach) — the march OWNS the fog on that span, the surface
// shaders' analytic applyFog only keeps the tail beyond it, and the
// tonemap composites `scene * a + rgb`. In-scatter per step = the sky
// haze (the analytic fog color) + the sun beam (V1 phase lobe) times the
// PER-STEP visibility (CSM + cloud shadows) — lit air glows, shadowed air
// darkens: fog as lighting instead of a grey veil.
layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;

#include "clouds.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec3 worldFromDepth(vec2 uv, float depth) {
    // 0..1 clip: the stored depth IS ndc z (no *2-1 remap).
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// Single hardware-compared CSM tap (no PCF — one per step per pixel).
float shaftShadow(vec3 p) {
    if (uShadowInfo.w <= 0.0) {
        return 1.0;
    }
    float d = distance(p, uCameraPos.xyz);
    if (d >= uCascadeSplits.z) {
        return 1.0;
    }
    int cascade = d < uCascadeSplits.x ? 0 : d < uCascadeSplits.y ? 1 : 2;
    vec4 lightClip = uSunViewProj[cascade] * vec4(p, 1.0);
    vec3 proj = lightClip.xyz / lightClip.w;
    proj.xy = proj.xy * 0.5 + 0.5; // 0..1 clip: only xy needs NDC->UV
    if (proj.z >= 1.0 || any(lessThan(proj.xy, vec2(0.0))) ||
        any(greaterThan(proj.xy, vec2(1.0)))) {
        return 1.0;
    }
    return texture(uShadowMap, vec4(proj.xy, float(cascade), proj.z));
}

void main() {
    float reach = uFogSunInfo.z;
    if (uTime.z <= 0.0 || reach <= 0.0 || uSunDirection.y <= -0.05) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0); // neutral: analytic fog owns
        return;
    }
    vec3 end = worldFromDepth(vUv, texture(uSceneDepth, vUv).r);
    vec3 ray = end - uCameraPos.xyz;
    float rayLen = min(length(ray), reach);
    vec3 dir = ray / max(length(ray), 1e-4);

    float start = uFogInfo.w; // clear air before the fog start
    float span = rayLen - start;
    if (span <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // The two in-scatter sources. The sun beam reuses the V1 lobe knobs
    // (strength per weather, exponent global); uTime.z (the Volumetric
    // shafts slider) stays the artistic multiplier of the BEAM only —
    // the haze term is the fog's physical color, not an effect.
    vec3 ambientAir = skyGradient(dir);
    float mu = dot(dir, uSunDirection.xyz);
    // Isotropic floor + forward lobe: the lobe carries the sunrise/sunset
    // glow toward the sun, the floor keeps midday cloud-gap curtains
    // visible SIDE-ON (mu ~ 0 there — a pure lobe extinguishes them).
    // hand-tuned floor.
    float lobe = pow(clamp(mu * 0.5 + 0.5, 0.0, 1.0), uFogSunInfo.y);
    float phase = 0.35 + 0.65 * lobe;
    vec3 sunAir = uSunColor.rgb * (phase * uFogSunInfo.x * uTime.z);

    const int kSteps = 20;
    float stepLen = span / float(kSteps);
    // Interleaved Gradient Noise (Jimenez): structured screen-space dither
    // that filters out smoothly — white noise here reads as ink blotches.
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                            0.00583715 * gl_FragCoord.y));

    float transmit = 1.0;
    vec3 inscatter = vec3(0.0);
    for (int i = 0; i < kSteps; ++i) {
        float d = start + (float(i) + jitter) * stepLen;
        vec3 p = uCameraPos.xyz + dir * d;
        float lowBoost =
            exp(-max(p.y - uTerrainInfo.x, 0.0) * uFogInfo.y);
        float density = uFogInfo.x * (1.0 + lowBoost * uFogInfo.z);
        float absorb = exp(-density * stepLen);
        float vis = cloudShadowFactor(p) * shaftShadow(p);
        // Shadowed air keeps a floor of haze (the sky still reaches it
        // sideways); the contrast between lit and shadowed air is what
        // draws the shafts and the dark curtains. hand-tuned.
        vec3 source = ambientAir * mix(0.45, 1.0, vis) + sunAir * vis;
        inscatter += transmit * source * (1.0 - absorb);
        transmit *= absorb;
        if (transmit < 0.003) {
            break; // opaque air: nothing behind can contribute
        }
    }

    fragColor = vec4(inscatter, transmit);
}
