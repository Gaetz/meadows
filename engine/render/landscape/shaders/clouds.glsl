// Stylized cloud layer + matching ground shadows (requires common.glsl).
// One noise field drives both: the clouds you see overhead are exactly the
// shadows drifting across the terrain.

float cloudHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float cloudNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = cloudHash(i);
    float b = cloudHash(i + vec2(1.0, 0.0));
    float c = cloudHash(i + vec2(0.0, 1.0));
    float d = cloudHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float cloudFbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += cloudNoise(p) * amplitude;
        p = p * 2.13 + vec2(31.7, 17.3);
        amplitude *= 0.5;
    }
    return sum;
}

// Cloud density [0,1] at a point of the (world-anchored) cloud plane. The
// pattern drifts with time — one global wind for layer and shadows alike.
// ANALYTIC evaluation: used by the sky dome (whose rays reach far beyond
// the baked field) and by the once-per-frame bake pass itself.
float cloudDensityAnalytic(vec2 planePos) {
    // Drift phase = accumulated wind time (uWindInfo.x), NOT wall time:
    // weather changing the wind speed must not teleport the pattern.
    vec2 wind = vec2(1.0, 0.35) * 17.0; // m/s of drift at wind strength 1
    vec2 uv = (planePos + wind * uWindInfo.x) * uCloudInfo.z;
    float f = cloudFbm(uv);
    float threshold = 1.0 - uCloudInfo.x * 0.9;
    return smoothstep(threshold - 0.18, threshold + 0.22, f);
}

#ifndef CLOUD_BAKE_PASS
// Baked cloud field (SkySystem renders it once per frame): every shadow
// consumer — terrain/tree/grass lighting and the 20-step volumetric march —
// reads ONE texture tap instead of a 4-octave FBM.
layout(binding = 2) uniform sampler2D uCloudMap;

float cloudDensityAt(vec2 planePos) {
    vec2 uv = (planePos - uCloudMapInfo.xy) * uCloudMapInfo.z + 0.5;
    return texture(uCloudMap, uv).r;
}
#endif

// Blends the cloud layer over the sky color for an upward view ray.
// Analytic density: horizon rays reach tens of km, far past the baked field.
vec3 applyClouds(vec3 sky, vec3 dir) {
    // With the volumetric clouds (skyclouds.frag) active the dome keeps
    // only the HORIZON band the raymarch hands off — the classical
    // far-cloud flattening, and both read the same coverage field so
    // the patterns agree across the seam. (The reflection pass has
    // cloudVolInfo.x = 0 and keeps the full 2D layer.)
    float domeShare = 1.0;
    if (uCloudVolInfo.x > 0.5) {
        domeShare = 1.0 - smoothstep(0.04, 0.10, dir.y);
        if (domeShare <= 0.0) {
            return sky;
        }
    }
    float horizonFade = smoothstep(0.006, 0.04, dir.y);
    if (horizonFade <= 0.0 || uCameraPos.y >= uCloudInfo.y) {
        return sky;
    }
    float t = (uCloudInfo.y - uCameraPos.y) / dir.y;
    vec2 planePos = uCameraPos.xz + dir.xz * t;
    float density = cloudDensityAnalytic(planePos);
    // Horizon LOD: at grazing angles the drifting pattern compresses
    // below pixel size (shimmer) while real cloud banks visually MERGE
    // through perspective — so the pattern's contrast fades toward its
    // coverage mean: a soft, stable distant cloud bank instead of the
    // old hard fade-to-empty.
    float bank = smoothstep(0.06, 0.015, dir.y);
    density = mix(density, clamp(uCloudInfo.x, 0.0, 1.0), bank * 0.85);
    if (density <= 0.0) {
        return sky;
    }
    // skyFill skies go dark and matte (the Ghibli storm-slab look — and the
    // backdrop volumetric shafts need); sparse skies keep bright clouds
    // with jewel edges.
    float skyFill = clamp(uCloudInfo.x, 0.0, 1.0);

    // Two-tone stylized shading, tinted by the day palette: white at noon,
    // rose/orange embers at dusk, faint slabs at night. The CORE darkening
    // scales with coverage: fair-weather cumulus
    // keep bright puffy interiors (0.80 of the lit tone); the storm-slab
    // darkness (0.38) only arrives as the sky fills in.
    vec3 lit = (uAmbientColor.rgb * 1.9 + uSunGlowColor.rgb * 0.34) *
               (1.0 - 0.35 * skyFill);
    vec3 core = lit * mix(0.80, 0.38, skyFill);
    float coreAmount = smoothstep(mix(0.35, 0.22, skyFill), 0.9, density);
    vec3 cloudColor = mix(lit, core, coreAmount);

    // Sparse-sky jewelry, fading out as the sky fills in:
    float sparse = 1.0 - 0.75 * skyFill;
    // Forward scattering: THIN cloud transmits sunlight, so looking toward
    // the sun the wispy parts glow — a broad soft lobe plus a tight halo
    // right around the disc.
    float sunAlign = max(dot(dir, uSunDirection.xyz), 0.0);
    float thin = 1.0 - coreAmount;
    float diffusion = pow(sunAlign, 8.0) * 0.5 + pow(sunAlign, 32.0) * 0.9;
    cloudColor += uSunGlowColor.rgb * (diffusion * thin * sparse);

    // Silver lining: the bright fringe where sun-facing cloud EDGES thin
    // out. Tinted by the live sun palette — silver at noon, gold/rose at
    // dawn and dusk.
    float edge = smoothstep(0.02, 0.20, density) *
                 (1.0 - smoothstep(0.25, 0.60, density));
    vec3 liningColor = uSunColor.rgb * 0.30 + uSunGlowColor.rgb * 0.30;
    cloudColor += liningColor * (edge * pow(sunAlign, 4.0) * sparse);

    return mix(sky, cloudColor, density * horizonFade * domeShare * 0.92);
}

// Sun attenuation from cloud cover, for terrain/vegetation lighting. The
// sample point is pushed along the sun ray up to the cloud layer, so shadows
// sit where the cloud actually blocks the sun (not straight above).
#ifndef CLOUD_BAKE_PASS
float cloudShadowFactor(vec3 worldPos) {
    if (uCloudInfo.w <= 0.0 || uSunDirection.y <= 0.08) {
        return 1.0;
    }
    float t = (uCloudInfo.y - worldPos.y) / uSunDirection.y;
    vec2 planePos = worldPos.xz + uSunDirection.xz * t;
    // Sharpened response: even a moderate cloud throws a solid patch — the
    // soft visual density would only dim the sun by a few percent.
    float shade = smoothstep(0.06, 0.55, cloudDensityAt(planePos));
    // Fade out at low sun: the projected sample point runs off the baked
    // field (offset ~ height/sin(elevation)), and grazing-light cloud
    // shadows are washed out anyway.
    float sunFade = smoothstep(0.08, 0.18, uSunDirection.y);
    return 1.0 - shade * uCloudInfo.w * sunFade;
}
#endif
