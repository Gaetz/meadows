// Chantier RC G6 — the GI apply (requires common.glsl). ONE branch point
// per surface shader: the ambient term becomes giAmbient(pos, n, classic).
// uGiInfo.x = 0 keeps the CLASSIC path byte-identical (the parallel-
// technique seam); 1 samples the merged cascade 0 — 8 hardware-trilinear
// slab fetches, cosine-weighted into irradiance — fading back to classic
// at the grid border and wherever the volume has no data.

layout(binding = 11) uniform sampler3D uGiCascade0;

// Direction of cascade-0 slab d — KEEP IN SYNC with rcOctDecode
// (rc_common.glsl) over the fixed 4x2 grid.
vec3 giSlabDir(int d) {
    vec2 cell = vec2(float(d % 4), float(d / 4));
    vec2 e = (cell + 0.5) / vec2(4.0, 2.0) * 2.0 - 1.0;
    vec3 v = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (v.y < 0.0) {
        vec2 sgn = vec2(v.x >= 0.0 ? 1.0 : -1.0, v.z >= 0.0 ? 1.0 : -1.0);
        vec2 flip = (1.0 - abs(vec2(v.z, v.x))) * sgn;
        v.x = flip.x;
        v.z = flip.y;
    }
    return normalize(v);
}

vec3 giAmbient(vec3 worldPos, vec3 normal, vec3 classicAmbient) {
    if (uGiInfo.x < 0.5) {
        return classicAmbient; // Classic: byte-identical
    }
    float res = uGiInfo.w;
    float spacing = uGiGridInfo.w;
    float span = res * spacing;
    vec3 uvw = (worldPos - uGiGridInfo.xyz) / span;

    // Fade to the classic ambient at the grid border (far-field fallback).
    vec3 edge = min(uvw, vec3(1.0) - uvw);
    float border = min(edge.x, min(edge.y, edge.z));
    float fade = clamp(border / max(uGiInfo.z / span, 0.001), 0.0, 1.0);
    if (fade <= 0.0) {
        return classicAmbient;
    }

    // 8 direction slabs, z clamped half a probe inside each slab so the
    // hardware trilinear never crosses into the next direction.
    float zProbe = clamp(uvw.z * res, 0.5, res - 0.5);
    vec2 uvXY = clamp(uvw.xy, vec2(0.5 / res), vec2(1.0 - 0.5 / res));
    vec3 sum = vec3(0.0);
    float weightSum = 0.0;
    for (int d = 0; d < 8; ++d) {
        float w = max(dot(normal, giSlabDir(d)), 0.0) + 0.05;
        float z = (zProbe + float(d) * res) / (res * 8.0);
        sum += w * texture(uGiCascade0, vec3(uvXY, z)).rgb;
        weightSum += w;
    }
    vec3 irradiance = sum / max(weightSum, 1e-3) * uGiInfo.y;

    // Stylized (BotW) mode — dev feedback 2026-07-11: the GI must speak
    // the same flat-pool language as the sun ramp, so it is posterized AT
    // THE END: luminance relative to the flat classic ambient snaps to
    // three bands (shade 0.55 / neutral 1.0 / bounce highlight 1.5), hue
    // preserved, narrow smoothsteps as anti-aliasing (stylized.glsl).
    float lum = dot(irradiance, vec3(0.299, 0.587, 0.114));
    float classicLum = dot(classicAmbient, vec3(0.299, 0.587, 0.114));
    if (lum > 1e-4 && classicLum > 1e-4) {
        float ratio = lum / classicLum;
        float stepped = 0.55 + 0.45 * smoothstep(0.70, 0.85, ratio) +
                        0.50 * smoothstep(1.30, 1.55, ratio);
        irradiance *= mix(1.0, (stepped * classicLum) / lum,
                          uAmbientColor.w);
    }
    return mix(classicAmbient, irradiance, fade);
}
