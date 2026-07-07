#version 460 core
#include "common.glsl"

// Grass redo (chantier 7.8) — the daniel-ilett/shaders-botw-grass look,
// ported to our instanced ribbons (the geometry-shader parts become
// vertex-shader math over the same 5-triangle tapered blade):
//  - WIND FIELD: a scrolling 2-octave world-space noise (the "scrolled
//    distortion texture" of the reference, procedural here) — gusts roll
//    across the meadow as organic waves instead of one global sine.
//  - CURVATURE: every blade arcs forward along its face direction with a
//    smooth profile, the reference's signature rounded silhouette.
//  - EDGE THICKENING: blades seen edge-on widen slightly so the meadow
//    never dissolves into hairlines.

layout(location = 0) in vec2 aBlade;    // x = side [-1,1] (taper baked), y = t
layout(location = 2) in vec4 aPosScale; // xyz = terrain point, w = height scale
layout(location = 3) in vec4 aParams;   // x = yaw, y = flutter phase,
                                        // z = tint jitter, w = lean
layout(location = 4) in vec4 aGroundNormal; // xyz = terrain normal (7.8bis)

out float vT;
out float vSide;
out float vTint;
out float vGust; // wind wave strength at this blade (tip lightening)
out vec3 vNormal;
out vec3 vWorldPos;

// 7.8 follow-up, matched to the reference's Properties: blade width
// 0.02-0.05 TOTAL (we draw half-width × the per-blade variation below),
// forward bend 0.38 with a pow(t, 2) curve.
const float kBladeHeight = 0.62;     // meters at scale 1
const float kBladeHalfWidth = 0.022; // -> 0.03-0.055 m total per blade

float hash21(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 s = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), s.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), s.x),
               s.y);
}

void main() {
    // Metric density LOD (perf: a grass wall seen from 20-100 m is
    // hundreds of thousands of SUBPIXEL triangles — one full 2x2 quad
    // per micro-triangle, the worst raster case). Fewer, WIDER blades
    // with distance: same visual mass, triangles stay near pixel size.
    // Keep key = flutter phase / 2pi (uniform per blade); the instance
    // buffer is SORTED by it, and GrassSystem::draw() cuts the prefix
    // with THIS SAME curve — keep the two in sync.
    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    float thin = smoothstep(18.0, 110.0, dist);
    float density = mix(1.0, 0.24, thin);
    if (aParams.y * 0.15915494 > density) {
        gl_Position = vec4(2.0, 0.0, 2.0, 1.0); // off-clip, zero raster
        return;
    }

    float t = aBlade.y;
    vT = t;
    vSide = aBlade.x;
    vTint = aParams.z;

    float yaw = aParams.x;
    vec3 sideDir = vec3(cos(yaw), 0.0, -sin(yaw));
    vec3 faceDir = vec3(sin(yaw), 0.0, cos(yaw));

    // Distance fade: blades shrink into the ground instead of popping.
    float fade = 1.0 - smoothstep(140.0, 190.0, dist);
    float height = kBladeHeight * aPosScale.w * fade;

    // The wind FIELD (the reference's scrolled distortion, procedural):
    // one broad wave layer + one busier layer, both drifting downwind.
    vec3 windDir = normalize(vec3(1.0, 0.0, 0.35));
    vec2 flow = aPosScale.xz * 0.055 - windDir.xz * (uWindInfo.x * 0.35);
    float wave = vnoise(flow) * 0.72 + vnoise(flow * 3.1 + 17.7) * 0.28;
    wave = wave * wave * 1.6; // gusts bite, lulls really rest
    float flutter =
        sin(uWindInfo.x * 5.3 + aParams.y * 6.2831853) * 0.05;
    float gust = (wave * 0.34 + flutter) * uWindInfo.y;
    vGust = clamp(wave * uWindInfo.y, 0.0, 1.0);

    // Curvature: pow(t, 2) forward arc (the reference's _BladeBendCurve
    // default), forward amount 0.38 ± the per-blade bend variation; wind
    // bends on top along its own direction.
    float curveT = t * t;
    vec3 bend = faceDir * ((0.38 * (0.8 + aParams.w * 0.4)) * curveT) +
                windDir * (gust * curveT);

    // Per-blade width variation (the reference's WidthMin..WidthMax) +
    // edge-on thickening so thin blades keep a body seen down the plane.
    vec3 toCam = normalize(uCameraPos.xyz - aPosScale.xyz);
    float edge = 1.0 - abs(dot(toCam.xz, normalize(faceDir.xz)));
    float width = kBladeHalfWidth * mix(0.7, 1.3, aParams.z) *
                  (1.0 + edge * 0.35) *
                  (1.0 + thin * 1.5); // density-LOD width compensation

    // 7.8ter — interactive bending (the Velorexe/BotW walk-through): the
    // player's feet push nearby blades outward and down; recovery is
    // implicit (the push follows the feet, blades spring back behind).
    if (uGrassBendInfo.w > 0.0) {
        vec2 away = aPosScale.xz - uGrassBendInfo.xy;
        float d = length(away);
        if (d < uGrassBendInfo.w &&
            abs(aPosScale.y - uGrassBendInfo.z) < 2.0) {
            float push = 1.0 - d / uGrassBendInfo.w;
            push *= push;
            bend += vec3(away.x / max(d, 0.05), 0.0,
                         away.y / max(d, 0.05)) *
                    (push * 0.9 * t);
            height *= 1.0 - push * 0.45; // squash under the step
        }
    }

    vec3 world = aPosScale.xyz + sideDir * (aBlade.x * width) +
                 vec3(0.0, height * t, 0.0) + bend * height;

    // 7.8bis — THE BotW trick: shade with the GROUND normal, barely
    // perturbed. The meadow lights as one continuous rolling surface
    // (like the terrain under it); blades exist as silhouettes and
    // motion, not as thousands of individually lit slivers.
    vNormal = normalize(aGroundNormal.xyz +
                        sideDir * (aBlade.x * 0.08) - bend * 0.15);

    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
