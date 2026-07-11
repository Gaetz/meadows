// Chantier RC G6 — the GI apply (requires common.glsl). ONE branch point
// per surface shader: the ambient term becomes giAmbient(pos, n, classic).
// uGiInfo.x = 0 keeps the CLASSIC path byte-identical (the parallel-
// technique seam); 1 samples the merged cascade 0 — 8 hardware-trilinear
// slab fetches, cosine-weighted into irradiance — fading back to classic
// at the grid border and wherever the volume has no data.

layout(binding = 11) uniform sampler3D uGiCascade0;

// Adaptive-ramp stats, measured by rc_adapt.comp on the merged cascade:
// x = log2 mean irradiance, y = contrast half-window (log2 stops),
// z = band count, w = 1 once initialized.
layout(std430, binding = 12) readonly buffer RcStatsBuf { vec4 uRcStats; };

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

    // ADAPTIVE stylized ramp (dev design 2026-07-11): posterize the GI in
    // log-stops anchored on the MEASURED scene range (rc_adapt.comp) —
    // a forest's subtle green bounce spreads across the flat pools, a
    // torch in the night keeps its full contrast; no fixed thresholds,
    // no reference to the (possibly black) classic ambient. Hue kept;
    // narrow smoothstep = anti-aliasing (the stylized.glsl language).
    float lum = dot(irradiance, vec3(0.299, 0.587, 0.114));
    if (uAmbientColor.w > 0.0 && uRcStats.w > 0.5 && lum > 1e-5) {
        float window = max(uRcStats.y, 0.1);
        float bands = max(uRcStats.z, 2.0);
        float x = clamp((log2(lum) - uRcStats.x) / window, -1.0, 1.0);
        float t = (x + 1.0) * 0.5 * (bands - 1.0); // 0 .. bands-1
        float tq = floor(t) + smoothstep(0.35, 0.65, fract(t));
        float xq = tq / (bands - 1.0) * 2.0 - 1.0;
        float lumQ = exp2(uRcStats.x + xq * window);
        irradiance *= mix(1.0, lumQ / lum, uAmbientColor.w);
    }
    return mix(classicAmbient, irradiance, fade);
}
