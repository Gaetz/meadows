#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Ground mist (docs/RENDERING.md §3.5): a separate raymarched medium that
// pools in terrain valleys — the world-erasing mist. Half-res march of
// (inscatter, transmittance) like volumetric.frag, but through an AUTHORED
// density field: the CPU-baked MistMap valley envelope (R = smoothed
// mist-top height, G = valleyness) gated by a wind-drifted coverage FBM
// (sparse patches — ridges and most valleys stay clear) and eroded by 3D
// noise. Composited by the tonemap at binding 4, BEFORE the fog term (the
// air fog veils distant mist, never the reverse).
layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;

#include "clouds.glsl"
#include "gi.glsl"
#include "volumetric_media.glsl"
#include "temporal_resolve.glsl"

layout(binding = 8) uniform sampler2D uMistMap;
layout(binding = 9) uniform sampler3D uNoiseVolume; // tileable Perlin-Worley
layout(binding = 10) uniform sampler2D uMistHistory; // last frame's target

// Temporal accumulation state (the froxel FroxelTemporalUniforms layout).
layout(std140, binding = 3) uniform MistTemporalUbo {
    mat4 uPrevViewProj;
    vec4 uPrevCamera;   // xyz = last frame's camera
    vec4 uTemporalInfo; // x = alpha (1 = no history), y = frame index
};

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

// Must match MistMap::kTopOffset / kTopRange / kValleyDepth quantization.
const float kTopOffset = -64.0;
const float kTopRange = 512.0;

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

// The mist ceiling at this column (baked smoothed terrain + live lift).
float mistTopAt(vec2 xz) {
    vec2 uv = (xz - uMistMapInfo.xy) * uMistMapInfo.z + 0.5;
    float r = texture(uMistMap, uv).r;
    return uTerrainInfo.x + kTopOffset + r * kTopRange + uMistShapeInfo.w;
}

// Coverage-noise uv: which valleys hold mist. Slow ground crawl (a
// quarter of the cloud drift) so patches wander without racing. The FBM
// is ~300 m scale, so the march evaluates it at the clipped segment's
// two ends only and lerps per step (the big per-step ALU saving).
vec2 coverageUv(vec3 p) {
    return (p.xz + vec2(1.0, 0.35) * (4.25 * uWindInfo.x)) *
           uMistShapeInfo.x;
}

// One noise tap, texture or analytic fallback. `q` is the TEXTURE-space
// coordinate (world × erosionScale/8 — the NoiseVolume's G channel tiles
// at freq 8, so world wavelength = 1/erosionScale on both paths).
float mistNoise(vec3 q) {
    return uMistDetailInfo.x > 0.5 ? texture(uNoiseVolume, q).g
                                   : mediaFbm3(q * 8.0);
}

// Local extinction at p; `coverNoise` = the pre-lerped coverage FBM;
// `segLen` drives the erosion's anti-flicker LOD (a long far segment
// under-samples the noise — its integral tends to the MEAN, so contrast
// fades with segLen instead of flickering under the depth jitter).
// Returns the (noise-domed) mist ceiling for the analytic sun path.
float mistDensityAt(vec3 p, float coverNoise, float segLen, out float top) {
    vec2 uv = (p.xz - uMistMapInfo.xy) * uMistMapInfo.z + 0.5;
    vec2 m = texture(uMistMap, uv).rg;
    // Ceiling softness scales with the lift: a 6 m transition under a
    // 50 m lift reads as a hard flat lid — parallelepiped mist.
    float soft = max(6.0, uMistShapeInfo.w * 0.5);
    float baseTop =
        uTerrainInfo.x + kTopOffset + m.r * kTopRange + uMistShapeInfo.w;
    top = baseTop;
    if (p.y > baseTop + soft * 0.5) {
        return 0.0; // above the tallest possible dome — no noise taps
    }
    float valley = smoothstep(0.08, 0.45, m.g);
    if (valley <= 0.0) {
        return 0.0;
    }
    float cover = smoothstep(uMistInfo.z - uMistInfo.w,
                             uMistInfo.z + uMistInfo.w, coverNoise);
    if (cover <= 0.0) {
        return 0.0;
    }
    vec3 drift = vec3(0.6, 0.1, 0.3) * (0.02 * uWindInfo.x);
    // LOW-frequency shape noise (quarter the erosion frequency) DOMES
    // the ceiling — density-only erosion can never carve the flat lid.
    // One tap, no distance dropout: it is what rounds the far masses,
    // and at 4x the wavelength it stays below the flicker limit.
    float shape =
        mistNoise(p * (uMistShapeInfo.y * 0.03125) + drift); // /8 /4
    top = baseTop + (shape - 0.5) * soft;
    // Soft ceiling; the floor is the terrain itself (scene depth ends
    // the ray) so no lower falloff is needed.
    float envelope = smoothstep(0.0, soft, top - p.y);
    if (envelope <= 0.0) {
        return 0.0;
    }
    // High-frequency erosion (Schneider remap): distance dropout AND
    // segment-length LOD both fade it toward its statistical mean.
    float erodedMean = (1.0 - uMistShapeInfo.z) * 0.5;
    float eroded = erodedMean;
    float detail = 1.0 - smoothstep(0.7 * uMistDetailInfo.z,
                                    uMistDetailInfo.z,
                                    distance(p, uCameraPos.xyz));
    detail *= clamp(1.0 / max(uMistShapeInfo.y * 2.0 * segLen, 1e-3),
                    0.0, 1.0); // wavelength / (2·segLen)
    if (detail > 0.0) {
        float n = mistNoise(p * (uMistShapeInfo.y * 0.125) + drift);
        eroded = mix(erodedMean, mediaErosionRemap(n, uMistShapeInfo.z),
                     detail);
    }
    float shape01 = envelope * valley * cover * eroded;
    // Fractal EDGE florets (the cloud recipe, ported): a finer billow
    // tap carves the patch borders into cauliflower lobes — the shapes
    // one imagines things in. Edge band only (cores stay solid), with
    // its own segment-length LOD (wavelength = base/3.7).
    float lodPuff = clamp(1.0 / max(uMistShapeInfo.y * 7.4 * segLen,
                                    1e-3),
                          0.0, 1.0);
    if (uMistPuffInfo.x > 0.0 && shape01 > 0.0 && shape01 < 0.35 &&
        detail > 0.0 && lodPuff > 0.02) {
        float fine =
            mistNoise(p * (uMistShapeInfo.y * 0.4625) + drift); // 3.7x
        fine = mix(0.5, fine, lodPuff);
        shape01 = mediaErosionRemap(shape01,
                                    (1.0 - fine) * uMistPuffInfo.x *
                                        (1.0 - shape01 / 0.35) * 0.8);
    }
    return uMistInfo.x * shape01;
}

void main() {
    if (uMistInfo.x <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0); // neutral: mist off
        return;
    }
    float reach = uMistInfo.y;
    vec3 end = worldFromDepth(vUv, texture(uSceneDepth, vUv).r);
    vec3 ray = end - uCameraPos.xyz;
    float rayLen = min(length(ray), reach);
    vec3 dir = ray / max(length(ray), 1e-4);

    // Horizontal slab clip: mist can only exist between the map's lowest
    // window and its highest baked top (+lift). Sky and peak pixels exit
    // here; the march spends its steps only inside the band.
    float slabTop = uMistMapInfo.w + uMistShapeInfo.w +
                    max(6.0, uMistShapeInfo.w * 0.5) * 0.5;
    float slabBot = uTerrainInfo.x + kTopOffset;
    float t0 = 0.0;
    float t1 = rayLen;
    if (abs(dir.y) > 1e-4) {
        float ta = (slabTop - uCameraPos.y) / dir.y;
        float tb = (slabBot - uCameraPos.y) / dir.y;
        t0 = max(min(ta, tb), 0.0);
        t1 = min(max(ta, tb), rayLen);
    } else if (uCameraPos.y > slabTop || uCameraPos.y < slabBot) {
        t1 = -1.0;
    }
    float span = t1 - t0;
    if (span <= 0.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Interleaved Gradient Noise (Jimenez), the volumetric.frag dither —
    // golden-ratio-rolled per frame so the EMA integrates it over time.
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                            0.00583715 * gl_FragCoord.y) +
                         uTemporalInfo.y * 0.61803398875);

    vec3 ambientAir = skyGradient(dir);
    float mu = dot(dir, uSunDirection.xyz);
    // Dual-lobe HG: forward silver lining toward the sun, soft
    // backscatter floor side-on — both lobes are live knobs
    // (uMistLightInfo.xy); the back lobe's g stays fixed.
    float phase = mediaDualLobeHg(mu, uMistLightInfo.x, -0.25,
                                  uMistLightInfo.y);
    // Grazing-sun clamp: bounds the analytic path length when the sun
    // sits low (grazing mist is ambient-dominated anyway, matching
    // cloudShadowFactor's own low-sun fade).
    float sunUp = max(uSunDirection.y, 0.25);

    // Coverage FBM at the clipped segment's ends (per-step it was the
    // dominant ALU cost; the pattern is ~300 m so a lerp is faithful).
    float coverNear = cloudFbm(coverageUv(uCameraPos.xyz + dir * t0));
    float coverFar = cloudFbm(coverageUv(uCameraPos.xyz + dir * t1));

    // Step count is a live knob (uMistDetailInfo.y): 16 by default —
    // the temporal EMA integrates what a single frame under-samples.
    int kSteps = int(max(uMistDetailInfo.y, 4.0));
    float transmit = 1.0;
    vec3 inscatter = vec3(0.0);
    for (int i = 0; i < kSteps; ++i) {
        // Quadratic step distribution over the clipped span: dense where
        // the ray enters the mist band, sparse at the far end. Each
        // segment carries its own length so transmittance stays exact.
        float s0 = (float(i) + jitter) / float(kSteps);
        float s1 = (float(i) + 1.0 + jitter) / float(kSteps);
        float d = t0 + span * s0 * s0;
        float segLen = span * (min(s1 * s1, 1.0) - s0 * s0);
        vec3 p = uCameraPos.xyz + dir * d;
        float top;
        float coverNoise = mix(coverNear, coverFar, s0 * s0);
        float density = mistDensityAt(p, coverNoise, segLen, top);
        if (density <= 0.0) {
            continue;
        }
        // Fade the last 20% of the reach so the mist never hard-clips.
        density *= 1.0 - smoothstep(0.8 * reach, reach, d);
        float absorb = exp(-density * segLen);
        // Per-step sun visibility: cloud shadows + CSM (mountains and
        // trees carve the mist like they carve the fog).
        float vis = cloudShadowFactor(p) * shaftShadow(p);
        // Analytic sun transmittance through the heightfield slab: path
        // length to exit the mist toward the sun, refined by ONE tap of
        // the ceiling at the estimated exit column — no light march.
        // Shadowed samples (CSM/cloud) skip the refinement: their sun
        // term is killed by `vis` anyway.
        float sunPath = (top - p.y) / sunUp;
        if (vis > 0.02) {
            float topExit = mistTopAt(p.xz + uSunDirection.xz * sunPath);
            sunPath = max((top + topExit) * 0.5 - p.y, 0.0) / sunUp;
        }
        float tauSun = density * sunPath;
        float sunT = exp(-tauSun);
        // Schneider powder, DIRECTIONAL (the repo's blend): full
        // dark-edge term looking away from the sun, none looking toward
        // it — applied flat it kills the silver lining exactly at the
        // thin lit rim where it lives.
        float powder = mix(2.0 * mediaPowder(tauSun), 1.0,
                           clamp(mu * 0.5 + 0.5, 0.0, 1.0));
        // The mist's own light: the GI field's air radiance (lamp glow,
        // canopy green) with a shadow floor, plus the boosted sun beam
        // (uMistDetailInfo.w — the normalized phase alone is too dim
        // against the full-sky ambient).
        vec3 haze = giAir(p, ambientAir);
        vec3 source =
            haze * (uMistLightInfo.z *
                    mix(uMistLightInfo.w, 1.0, vis)) +
            uSunColor.rgb *
                (phase * sunT * powder * vis * uMistDetailInfo.w);
        inscatter += transmit * source * (1.0 - absorb);
        transmit *= absorb;
        if (transmit < 0.003) {
            break; // opaque mist: nothing behind contributes
        }
    }

    // Temporal resolve: reproject the WORLD point at the pixel's depth
    // into last frame's view and EMA against the history copy. NEAR
    // geometry takes little to no history: its reprojection error is the
    // largest (parallax) and moving foreground objects — the carried
    // sword, grass tips against the sky — would drag mist ghosts;
    // there is barely any mist in front of it to denoise anyway.
    vec4 current = vec4(inscatter, transmit);
    float histWeight = smoothstep(4.0, 25.0, rayLen);
    float alpha = mix(1.0, uTemporalInfo.x, histWeight);
    if (alpha < 1.0) {
        vec2 prevUv = temporalReprojectUv(end, uPrevViewProj);
        if (prevUv.x >= 0.0) {
            float tolScale = mix(1.0, 3.0,
                                 smoothstep(150.0, 500.0, rayLen));
            current = temporalResolve(current,
                                      texture(uMistHistory, prevUv),
                                      alpha, tolScale);
        }
    }
    fragColor = current;
}
