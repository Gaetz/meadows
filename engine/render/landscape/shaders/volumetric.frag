#version 460 core
#include "common.glsl"

// Cheap volumetric light shafts (half-res, 14 jittered steps): march the
// view ray through the air, accumulating sunlight where neither the clouds
// (analytic shadow) nor the geometry (one CSM tap per step) block it. Denser
// near the ground — distant valleys catch Ghibli light curtains.
layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;

#include "clouds.glsl"

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// Single hardware-compared CSM tap (no PCF — 14 of these per pixel).
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
    vec3 proj = lightClip.xyz / lightClip.w * 0.5 + 0.5;
    if (proj.z >= 1.0 || any(lessThan(proj.xy, vec2(0.0))) ||
        any(greaterThan(proj.xy, vec2(1.0)))) {
        return 1.0;
    }
    return texture(uShadowMap, vec4(proj.xy, float(cascade), proj.z));
}

void main() {
    // Alpha is a MULTIPLIER over the scene (1 = neutral) — see below.
    if (uTime.z <= 0.0 || uSunDirection.y <= -0.05) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 end = worldFromDepth(vUv, texture(uSceneDepth, vUv).r);
    vec3 ray = end - uCameraPos.xyz;
    float rayLen = min(length(ray), 1400.0); // march reach (far columns)
    vec3 dir = ray / max(length(ray), 1e-4);

    // Mild forward phase: shafts glow toward the sun yet stay visible
    // side-on (that's the whole point).
    float mu = dot(dir, uSunDirection.xyz);
    float phase = 0.4 + 0.6 * pow(mu * 0.5 + 0.5, 3.0);

    const int kSteps = 20;
    float stepLen = rayLen / float(kSteps);
    // Interleaved Gradient Noise (Jimenez): structured screen-space dither
    // that filters out smoothly — white noise here reads as ink blotches.
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                            0.00583715 * gl_FragCoord.y));

    // Two accumulators, two effects:
    //  - NEAR field (transmittance-weighted): ADDITIVE shafts, the air the
    //    fog has not eaten yet.
    //  - FAR field (weighted by where the fog's in-scatter is born,
    //    density x transmittance): a MULTIPLIER that removes the unshadowed
    //    in-scatter the fog already added where the marched air turns out
    //    to be cloud-shadowed. Dark distant curtains carve out the bright
    //    corridors left untouched -> distant Ghibli shafts by contrast,
    //    with no double-counted light.
    float accumLit = 0.0;
    float accumMax = 0.0;
    float fogLit = 0.0;
    float fogMax = 0.0;
    float opticalDepth = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        float d = (float(i) + jitter) * stepLen;
        vec3 p = uCameraPos.xyz + dir * d;
        // The scattering medium thins with altitude: shafts live low.
        float heightFall = exp(-max(p.y - uTerrainInfo.x, 0.0) * 0.004);
        float lowBoost =
            exp(-max(p.y - uTerrainInfo.x, 0.0) * uFogInfo.y);
        float fogDensity = uFogInfo.x * (1.0 + lowBoost * uFogInfo.z);
        float transmit = exp(-max(d - uFogInfo.w, 0.0) * fogDensity);
        float lit = cloudShadowFactor(p) * shaftShadow(p);

        // Column DETECTION reaches farther than the physical transmittance
        // (sqrt slows the falloff): safe now that the additive brightness is
        // fixed/capped — extending the reach can't rebuild the white veil.
        float weight = heightFall * sqrt(transmit);
        accumLit += lit * weight;
        accumMax += weight;

        float fogWeight = fogDensity * transmit;
        fogLit += lit * fogWeight;
        fogMax += fogWeight;
        if (d > uFogInfo.w) {
            opticalDepth += fogDensity * stepLen;
        }
    }

    // User-requested gate: volumetric shafts belong to genuinely cloudy
    // skies (ramp in between 30% and 40% coverage).
    float gate = smoothstep(0.30, 0.40, uCloudInfo.x) * uTime.z;

    // ADDITIVE near shafts: absolute excess above the expected average ray,
    // remapped through a steep smoothstep for DEFINED column borders, and
    // with a FIXED capped luminance (decoupled from ray length) — a uniform,
    // readable addition that never buries what's behind it.
    float litRatio = accumLit / max(accumMax, 1e-3);
    float meanLit =
        clamp(1.0 - uCloudInfo.x * uCloudInfo.w * 0.75, 0.05, 0.98);
    float excess = max(litRatio - meanLit, 0.0);
    float column = smoothstep(0.15, 0.40, excess); // tight = crisp borders
    float reach = min(rayLen / 250.0, 1.0); // very short rays: no full shaft
    vec3 shaft =
        uSunGlowColor.rgb * (column * 0.28 * phase * reach * gate);

    // MULTIPLICATIVE far curtains: darken the fog's in-scatter where the
    // distant air is shadowed, remapped into clean BANDS (the raw ratio
    // smears like spilled ink), scaled by how much fog covers this pixel.
    float fogAmount = 1.0 - exp(-opticalDepth);
    float litRatioFog = fogLit / max(fogMax, 1e-5);
    float band = smoothstep(0.18, 0.55, 1.0 - litRatioFog);
    float darken = fogAmount * band * 0.80 * gate;
    float fogShadowMul = clamp(1.0 - darken, 0.0, 1.0);

    fragColor = vec4(shaft, fogShadowMul);
}
