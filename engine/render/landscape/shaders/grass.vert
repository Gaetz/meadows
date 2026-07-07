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

out float vT;
out float vSide;
out float vTint;
out float vGust; // wind wave strength at this blade (tip lightening)
out vec3 vNormal;
out vec3 vWorldPos;

const float kBladeHeight = 0.68;    // meters at scale 1
const float kBladeHalfWidth = 0.05;

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
    float t = aBlade.y;
    vT = t;
    vSide = aBlade.x;
    vTint = aParams.z;

    float yaw = aParams.x;
    vec3 sideDir = vec3(cos(yaw), 0.0, -sin(yaw));
    vec3 faceDir = vec3(sin(yaw), 0.0, cos(yaw));

    // Distance fade: blades shrink into the ground instead of popping.
    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
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

    // Curvature: the blade arcs forward (t^1.5 keeps the root planted and
    // rounds the tip), lean gives each blade its own settled arc; wind
    // bends on top along its own direction.
    float curveT = t * sqrt(t);
    vec3 bend = faceDir * ((0.22 + aParams.w * 0.35) * curveT) +
                windDir * (gust * curveT);

    // Edge-on thickening: when the camera looks down the blade's plane,
    // widen it so it keeps a visible body (the geoshader trick, cheap).
    vec3 toCam = normalize(uCameraPos.xyz - aPosScale.xyz);
    float edge = 1.0 - abs(dot(toCam.xz, normalize(faceDir.xz)));
    float width = kBladeHalfWidth * (1.0 + edge * 0.6);

    vec3 world = aPosScale.xyz + sideDir * (aBlade.x * width) +
                 vec3(0.0, height * t, 0.0) + bend * height;

    // Rounded-ribbon normal: mostly up, tilted by the side coordinate so
    // the two halves catch light differently (fake cylinder shading),
    // pushed over with the bend.
    vNormal = normalize(vec3(0.0, 1.0, 0.0) + sideDir * (aBlade.x * 0.55) -
                        (bend * 0.9));

    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
