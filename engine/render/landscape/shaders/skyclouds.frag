#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Volumetric sky clouds (docs/RENDERING.md §8 contract, built on the
// ground-mist socle): ½-res march of an altitude slab
// [cloudInfo.y, +cloudVolInfo.y]. The COVERAGE is the same analytic
// field as the 2D dome and the shadow bake (cloudDensityAnalytic), so
// the baked ground shadows match the volumetric clouds by construction
// — the §8 seam (cloudShadowFactor) does not move. Shape = coverage ×
// vertical profile × Perlin-Worley erosion (NoiseVolume R channel, the
// lane reserved for the sky); lighting = short sun march fed to the
// multi-octave scattering approximation + directional powder + a
// height-graded ambient. Temporal EMA like the mist. The tonemap
// composites this FIRST (binding 7) — clouds are the farthest medium,
// mist and fog veil them.
#include "clouds.glsl" // full version: the under-cloud ray march taps
                       // cloudShadowFactor (the baked 512² map)
#include "volumetric_media.glsl"
#include "temporal_resolve.glsl"

layout(binding = 0) uniform sampler2D uSceneDepth;
layout(binding = 9) uniform sampler3D uNoiseVolume;
layout(binding = 10) uniform sampler2D uCloudsHistory;

layout(std140, binding = 3) uniform CloudTemporalUbo {
    mat4 uPrevViewProj;
    vec4 uPrevCamera;
    vec4 uTemporalInfo; // x = alpha (1 = no history), y = frame index
};

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

// NoiseVolume R tile mapped so its freq-4 Perlin-Worley billows span
// ~400 m — cumulus-scale features. hand-tuned.
const float kShapeScale = 1.0 / 1600.0;
// Billow wavelength = tile/freq8 = 200 m; florets at 3.7x = ~54 m. The
// sampling LOD below fades each octave once a segment exceeds half its
// wavelength (the mist lesson: under-sampled noise + jitter = flicker;
// its integral limit is the MEAN).
const float kBillowHalfWave = 100.0;
const float kFloretHalfWave = 27.0;
const int kStepsMax = 40;
const int kLightSteps = 3;

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// Local extinction. `cover` = the shared 2D coverage field at p.xz;
// `thick` = the frame's effective slab thickness (coverage-scaled);
// `detailAmt` = the puffiness edge pass (0 for the light march — the
// Nubis practice: light samples use the coarse density); `segLen` =
// the caller's segment length, driving the anti-flicker sampling LOD.
float cloudDensityVol(vec3 p, float cover, float thick, float detailAmt,
                      float segLen) {
    if (cover <= 0.0) {
        return 0.0;
    }
    float h = (p.y - uCloudInfo.y) / thick;
    if (h <= 0.0 || h >= 1.0) {
        return 0.0;
    }
    vec3 windOfs = vec3(1.0, 0.0, 0.35) * (17.0 * uWindInfo.x);
    // Per-column TOP variation (a fixed-z volume tap ≈ tileable 2D
    // noise): tops rise and dip independently of coverage — the Nubis
    // height gradient. Kills the single flat lid.
    float topVar =
        texture(uNoiseVolume,
                vec3((p.xz + windOfs.xz) * (kShapeScale * 0.5), 0.37)).r;
    // Rounded vertical profile: quick rise off the flat-ish base, domed
    // top that climbs with coverage (dense cores tower higher).
    float topH = mix(0.5, 1.0, cover) * mix(0.55, 1.0, topVar);
    float profile = smoothstep(0.0, 0.12, h) *
                    (1.0 - smoothstep(topH * 0.5, topH, h));
    float shaped = cover * profile;
    if (shaped <= 0.0) {
        return 0.0;
    }
    // Height-mixed erosion (the Nubis signature): WISPY Perlin-Worley
    // at the base, round WORLEY BILLOWS toward the top — one RGB tap
    // carries both frequencies. Drifts with the coverage wind.
    float lodBase = clamp(kBillowHalfWave / max(segLen, 1.0), 0.0, 1.0);
    // Curl-style domain warp (the Nubis turbulence, in its cheap
    // domain-warp form): a low-frequency vector field bends the erosion
    // sampling domain so the texture SWIRLS instead of reading as a
    // static decal; the slow vertical advection makes the edges boil.
    // View march only, amplitude rides the puffiness knob, and the
    // sampling LOD fades it with the erosion it feeds.
    vec3 warp = vec3(0.0);
    if (detailAmt > 0.0 && lodBase > 0.02) {
        vec3 warpCoord = (p + windOfs) * (kShapeScale * 0.35) +
                         vec3(0.0, uTime.x * 0.004, 0.0);
        warp = (texture(uNoiseVolume, warpCoord).rgb - 0.5) *
               (90.0 * detailAmt * lodBase);
    }
    vec3 noise =
        texture(uNoiseVolume, (p + windOfs + warp) * kShapeScale).rgb;
    float baseErode = mix(noise.r, noise.g, clamp(h * 1.4, 0.0, 1.0));
    baseErode = mix(0.5, baseErode, lodBase);
    shaped = mediaErosionRemap(shaped, (1.0 - baseErode) * uCloudVolInfo.w);
    if (shaped <= 0.0) {
        return 0.0;
    }
    // Fractal EDGE pass (view march only): a finer billow tap carves
    // the silhouette into cauliflower florets. Fires only in the
    // low-density edge band, so the cores stay cheap and solid.
    float lodFine = clamp(kFloretHalfWave / max(segLen, 1.0), 0.0, 1.0);
    if (detailAmt > 0.0 && shaped < 0.35 && lodFine > 0.02) {
        float fine = texture(uNoiseVolume,
                             (p + windOfs + warp) * (kShapeScale * 3.7)).g;
        fine = mix(0.5, fine, lodFine);
        shaped = mediaErosionRemap(shaped,
                                   (1.0 - fine) * detailAmt *
                                       (1.0 - shaped / 0.35) * 0.8);
    }
    return shaped * uCloudVolInfo.z;
}

void main() {
    if (uCloudVolInfo.x <= 0.5) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0); // neutral: feature off
        return;
    }
    float depthS = texture(uSceneDepth, vUv).r;
    vec3 end = worldFromDepth(vUv, depthS);
    vec3 ray = end - uCameraPos.xyz;
    float rayLen = length(ray);
    vec3 dir = ray / max(rayLen, 1e-4);
    // SKY pixels: the depth reconstruction lands on the far plane, which
    // sits closer than the slanted slab entry — without this, clouds
    // exist only overhead (t0 small) and vanish toward the horizon.
    if (depthS >= 0.9999) {
        rayLen = 1.0e8;
    }

    // Slab intersection (camera below the layer in normal play) + the
    // dome's horizon fade — grazing rays dissolve into the haze.
    // Effective thickness scales with the WEATHER's coverage: filling
    // skies tower (storm slabs), sparse skies stay shallow cumulus.
    float base = uCloudInfo.y;
    float thick = uCloudVolInfo.y *
                  mix(max(1.0 - uCloudVolShapeInfo.x, 0.15),
                      1.0 + uCloudVolShapeInfo.x,
                      clamp(uCloudInfo.x, 0.0, 1.0));
    float top = base + thick;
    // Below this band the 2D dome takes over (the classical far-cloud
    // flattening: a grazing slab subtends no vertical angle — one 2D
    // sample says it all, and the dome IS that sample, on the same
    // coverage field). The two fades are complementary.
    float horizonFade = smoothstep(0.04, 0.10, dir.y);
    if (horizonFade <= 0.0 || uCameraPos.y >= base || dir.y <= 1e-3) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float t0 = (base - uCameraPos.y) / dir.y;
    float t1 = (top - uCameraPos.y) / dir.y;
    float span = min(min(t1, rayLen) - t0, 15000.0);
    bool marchSlab = rayLen >= t0 && span > 0.0;

    // Golden-ratio-rolled IGN (the mist pattern) for the temporal EMA.
    float jitter = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                            0.00583715 * gl_FragCoord.y) +
                         uTemporalInfo.y * 0.61803398875);

    float mu = dot(dir, uSunDirection.xyz);
    float sunUp = max(uSunDirection.y, 0.25);
    float transmit = 1.0;
    vec3 inscatter = vec3(0.0);

    // (A far "sky fog tail" march lived here 2026-07-30 and was REMOVED:
    // after its containments it only lit the air BETWEEN the clouds —
    // negative visual value. The ground-to-cloud ray curtains are the
    // froxels' job; beyond their reach the horizon belongs to applyFog.)

    // Step count targets a fixed ~40 m segment (bounded, so tall storm
    // slabs keep resolving their 200 m billows instead of jittering
    // across them), capped for grazing spans — the sampling LOD in
    // cloudDensityVol absorbs whatever the cap under-samples. Geometry
    // before the slab skips it (the air rays above still apply).
    int steps = marchSlab
                    ? int(clamp(span / 40.0, 8.0, float(kStepsMax)))
                    : 0;
    float stepLen = marchSlab ? span / float(steps) : 0.0;
    for (int i = 0; i < steps; ++i) {
        float d = t0 + (float(i) + jitter) * stepLen;
        vec3 p = uCameraPos.xyz + dir * d;
        float cover = cloudDensityAnalytic(p.xz);
        float density = cloudDensityVol(p, cover, thick,
                                        uCloudVolShapeInfo.w, stepLen);
        if (density <= 0.0) {
            continue;
        }
        float absorb = exp(-density * stepLen);
        // Short sun march: 3 taps toward the slab exit, coverage PER
        // TAP — a mid-only tap reported thick cloud at the edges and
        // killed the silver lining exactly where it lives. Distant
        // samples drop to ONE tap (the ½-res pixel out there covers a
        // whole cloud anyway).
        int lightSteps = d > 3000.0 ? 1 : kLightSteps;
        float lightLen = min((top - p.y) / sunUp, thick * 1.5);
        float lstep = lightLen / float(lightSteps);
        float tauSun = 0.0;
        for (int j = 0; j < lightSteps; ++j) {
            vec3 lp = p + uSunDirection.xyz * ((float(j) + 0.5) * lstep);
            tauSun += cloudDensityVol(lp, cloudDensityAnalytic(lp.xz),
                                      thick, 0.0, lstep) *
                      lstep;
        }
        // STORM DIMMING, exponential (darkness perception is log — a
        // linear knob on exp(-x) gives even control all the way to
        // black): the whole cloud falls with the weather's coverage the
        // way the ground ambient does, bases fastest. It dims the
        // AMBIENT fully and the sun BODY at 85% — only the edge
        // phenomena (lining, rim) keep their fire: black slabs rimmed
        // with light is the stormy-sunset look.
        float h = clamp((p.y - base) / thick, 0.0, 1.0);
        float storm = uCloudVolRimInfo.z * clamp(uCloudInfo.x, 0.0, 1.0);
        float dim = exp(-storm * (0.6 + 0.8 * (1.0 - h)));
        // TWO sun terms: the multi-octave BODY (progressively isotropic
        // by construction — luminous cores, no rim) and the
        // direct-transmission LINING exp(-tau)·HG(high g), which
        // explodes at thin sun-facing edges — THE silver lining.
        float body =
            mediaMultiOctaveScattering(tauSun, mu, uCloudVolLightInfo.y);
        float lining =
            exp(-tauSun) * mediaHgPhase(mu, uCloudVolShapeInfo.y);
        float powder = mix(2.0 * mediaPowder(tauSun), 1.0,
                           clamp(mu * 0.5 + 0.5, 0.0, 1.0));
        powder = mix(1.0, powder, uCloudVolShapeInfo.z);
        vec3 sunTerm =
            uSunColor.rgb *
            ((body * uCloudVolLightInfo.x * mix(1.0, dim, 0.85) +
              lining * uCloudVolLightInfo.w) *
             powder);
        vec3 ambient = skyGradient(dir) *
                       (uCloudVolLightInfo.z * mix(0.45, 1.1, h) * dim);
        vec3 source = ambient + sunTerm;
        inscatter += transmit * source * (1.0 - absorb);
        transmit *= absorb;
        if (transmit < 0.005) {
            break;
        }
    }

    // Silhouette RIM: strongly forward-scattered sun through the pixels
    // where the cloud is thin along the VIEW ray (intermediate final
    // transmittance = the silhouette band). The sun-path lining cannot
    // produce this — its tau knows nothing of the view thickness.
    float rimBand = smoothstep(0.15, 0.55, transmit) *
                    (1.0 - smoothstep(0.85, 0.995, transmit));
    inscatter += uSunColor.rgb *
                 (rimBand * mediaHgPhase(mu, uCloudVolRimInfo.y) *
                  uCloudVolRimInfo.x);

    vec4 current = vec4(inscatter, transmit);
    current = mix(vec4(0.0, 0.0, 0.0, 1.0), current, horizonFade);

    // Temporal resolve anchored on the slab-entry world point (clouds
    // are world-fixed; the entry point reprojects correctly under
    // camera motion). Distant medium: no near-ghosting concern, use the
    // wide clamp so the EMA converges.
    float alpha = uTemporalInfo.x;
    if (alpha < 1.0) {
        vec3 anchor =
            uCameraPos.xyz +
            dir * min(t0 + max(span, 0.0) * 0.3, 12000.0);
        vec2 prevUv = temporalReprojectUv(anchor, uPrevViewProj);
        if (prevUv.x >= 0.0) {
            current = temporalResolve(current,
                                      texture(uCloudsHistory, prevUv),
                                      alpha, 3.0);
        }
    }
    fragColor = current;
}
