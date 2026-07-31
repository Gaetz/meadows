#version 460 core
#include "common.glsl"
#include "sky.glsl"

// The fog INTEGRATOR (docs/RENDERING.md V2): half-res, jittered march of
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
#include "gi.glsl"
#include "view_util.glsl"
#include "fog_march.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

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
    vec3 sunAir = uSunColor.rgb * (fogSunPhase(mu) * uFogSunInfo.x * uTime.z);

    const int kSteps = 20;
    float jitter = ignJitter(gl_FragCoord.xy);

    float transmit = 1.0;
    vec3 inscatter = vec3(0.0);
    vec3 haze = vec3(0.0);
    for (int i = 0; i < kSteps; ++i) {
        // QUADRATIC step distribution: dense near the camera (several
        // steps inside the ~32 m RC volume — where giAir varies), sparse
        // in the far haze (smooth anyway). Transmittance stays exact:
        // each segment uses its own length.
        float t0 = (float(i) + jitter) / float(kSteps);
        float t1 = (float(i) + 1.0 + jitter) / float(kSteps);
        float d = start + span * t0 * t0;
        float segLen = span * (min(t1 * t1, 1.0) - t0 * t0);
        vec3 p = uCameraPos.xyz + dir * d;
        float lowBoost =
            exp(-max(p.y - uTerrainInfo.x, 0.0) * uFogInfo.y);
        float density = uFogInfo.x * (1.0 + lowBoost * uFogInfo.z) *
                        exp(-max(p.y - uTerrainInfo.x, 0.0) *
                            uFogLayerInfo.x);
        float absorb = exp(-density * segLen);
        float vis = cloudShadowFactor(p) * shaftShadow(p);
        // V3: inside the RC volume the haze takes the FIELD's radiance —
        // green under a canopy clearing, dark in a shaded valley, lamp
        // glows in night mist; outside, the sky gradient as before.
        // Sampled once per step PAIR and held: giAir's 8 slab fetches are
        // the step's dominant cost, and the field is trilinear over
        // metre-scale probes — a one-step hold stays under its own
        // filtering radius.
        if ((i & 1) == 0) {
            haze = giAir(p, ambientAir);
        }
        // Shadowed air keeps a floor of haze (the sky still reaches it
        // sideways); the contrast between lit and shadowed air is what
        // draws the shafts and the dark curtains. hand-tuned.
        vec3 source =
            haze * mix(0.45, 1.0, vis) + sunAir * (vis * fogSunLift(p.y));
        inscatter += transmit * source * (1.0 - absorb);
        transmit *= absorb;
        if (transmit < 0.003) {
            break; // opaque air: nothing behind can contribute
        }
    }

    fragColor = vec4(inscatter, transmit);
}
