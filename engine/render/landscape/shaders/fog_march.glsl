// Shared helpers for the raymarched media passes (requires common.glsl
// and a `uShadowMap` sampler2DArrayShadow declared BEFORE this include —
// the sky.glsl/clouds.glsl ordering convention).

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

// The height-fog sun lobe (V1 knobs): isotropic floor + forward lobe —
// the lobe carries the sunrise/sunset glow toward the sun, the floor
// keeps midday cloud-gap curtains visible SIDE-ON (mu ~ 0 there — a
// pure lobe extinguishes them). hand-tuned floor.
float fogSunPhase(float mu) {
    float lobe = pow(clamp(mu * 0.5 + 0.5, 0.0, 1.0), uFogSunInfo.y);
    return 0.35 + 0.65 * lobe;
}

// The fog SUN term's altitude envelope, 3x softer than the extinction's
// ceiling and fading toward the cloud base (the froxel contract,
// docs/RENDERING.md §3.5): the sky stays readable while the
// cloud-shadow ray curtains keep their medium at curtain altitudes.
float fogSunLift(float y) {
    return exp(max(y - uTerrainInfo.x, 0.0) * (uFogLayerInfo.x * 0.65)) *
           (1.0 - smoothstep(uCloudInfo.y * 0.45, uCloudInfo.y * 1.0, y));
}
