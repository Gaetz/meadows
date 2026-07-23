// The GI apply (requires common.glsl). ONE branch point
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

// Direction-AVERAGED radiance for AIR (docs/VOLUMETRIC.md V3): the fog's
// ambient in-scatter inside the RC volume — same grid and border fade as
// giAmbient, but no normal weighting (air sees every direction), no
// banding, no floor (air is not a surface). Outside the grid or with RC
// off: the classic haze the caller passes.
vec3 giAir(vec3 worldPos, vec3 classicAmbient) {
    if (uGiInfo.x < 0.5) {
        return classicAmbient;
    }
    float res = uGiInfo.w;
    float spacing = uGiGridInfo.w;
    float span = res * spacing;
    vec3 uvw = (worldPos - uGiGridInfo.xyz) / span;
    vec3 edge = min(uvw, vec3(1.0) - uvw);
    float border = min(edge.x, min(edge.y, edge.z));
    float fade = clamp(border / max(uGiInfo.z / span, 0.001), 0.0, 1.0);
    if (fade <= 0.0) {
        return classicAmbient;
    }
    float zProbe = clamp(uvw.z * res, 0.5, res - 0.5);
    vec2 uvXY = clamp(uvw.xy, vec2(0.5 / res), vec2(1.0 - 0.5 / res));
    vec3 sum = vec3(0.0);
    for (int d = 0; d < 8; ++d) {
        float z = (zProbe + float(d) * res) / (res * 8.0);
        sum += texture(uGiCascade0, vec3(uvXY, z)).rgb;
    }
    return mix(classicAmbient, sum * (uGiInfo.y / 8.0), fade);
}

vec3 giAmbient(vec3 worldPos, vec3 normal, vec3 classicAmbient) {
    if (uGiInfo.x < 0.5) {
        return classicAmbient; // Classic: byte-identical
    }
    float res = uGiInfo.w;
    float spacing = uGiGridInfo.w;
    float span = res * spacing;
    // Sample ONE probe step off the surface: at the terrain floor, half
    // the trilinear neighbors are BURIED probes — black, beta 0 — which
    // would crush the ground's GI toward the dim band while walls and
    // interiors band fine. Off-surface, the clean air
    // probes carry the full range, and the downward directions see the
    // lit floor: the ground gets its own bounce back.
    vec3 samplePos = worldPos + normal * spacing;
    vec3 uvw = (samplePos - uGiGridInfo.xyz) / span;

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

    // FIXED log-step posterization: the GI luminance snaps to ABSOLUTE
    // exposure steps of uGiBandInfo.x stops. (An adaptive ramp anchored
    // on the measured scene range was rejected: it measured the AIR,
    // moved with the weather and fought the multi-bounce.) Predictable
    // by design — bands appear wherever the GI varies by one step, day,
    // night and torch rings alike; hue kept; narrow smoothstep = AA
    // (the stylized.glsl language), uGiBandInfo.y = AA width.
    // The GI floor (uGiBandInfo.z): RC never drops below this fraction of
    // the CLASSIC ambient — the two illumination models meet seamlessly at
    // the grid border, and occluded areas (canopies, rooms) stay readable
    // while classic ambient remains the artistic lever (per weather,
    // interior, time of day). BEFORE the banding, so the bands quantize
    // the LIVING range above the floor (floor-after flattened most of the
    // banded range and played the band knobs dead); re-asserted after,
    // since a band rounds down by up to one full step.
    vec3 floorAmbient = classicAmbient * uGiBandInfo.z;
    irradiance = max(irradiance, floorAmbient);
    // Optional posterization, uGiBandInfo.x = BAND COUNT (0 = smooth, the
    // default — the BotW/Genshin reference keeps ambient smooth and puts
    // the cel ramp on the direct term). N flat bands between the floor
    // and the CLASSIC ambient — anchored to the artistic value (weather,
    // interior, hour), never to a measured scene range (the adaptive-ramp
    // lesson, docs/RADIANCE-CASCADES.md); the same step continues above
    // classic so lamp glows band consistently.
    float bands = uGiBandInfo.x;
    float lum = dot(irradiance, vec3(0.299, 0.587, 0.114));
    if (uAmbientColor.w > 0.0 && bands >= 1.0 && lum > 1e-5) {
        float classicLum =
            dot(classicAmbient, vec3(0.299, 0.587, 0.114));
        float floorLum = classicLum * uGiBandInfo.z;
        float bandStep = max((classicLum - floorLum) / bands, 1e-4);
        float aa = clamp(uGiBandInfo.y, 0.02, 0.49);
        float t = (lum - floorLum) / bandStep;
        float tq = floor(t) + smoothstep(0.5 - aa, 0.5 + aa, fract(t));
        float lumQ = floorLum + tq * bandStep;
        irradiance *= mix(1.0, max(lumQ, 0.0) / lum, uAmbientColor.w);
        irradiance = max(irradiance, floorAmbient);
    }
    return mix(classicAmbient, irradiance, fade);
}
