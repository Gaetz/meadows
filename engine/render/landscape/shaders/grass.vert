#version 460 core
#include "common.glsl"

// The SimonDev Quick_Grass
// model (github.com/simondevyoutube/Quick_Grass), ported from three.js to
// our instanced scatter. Every
// shaping constant lives in the FrameUbo (uGrassShapeInfo/uGrassLod-
// Info/uGrass*Color), driven live from the render panel's "Grass"
// category. Per blade:
//  - SHAPE: 6-segment strip, width tapered 1-t^2 (rounded tip) near,
//    linear far; the blade ARCS forward by rotating each vertex around
//    the blade's side axis by an angle growing with eased height.
//  - WIND: two independent noises — a slow DIRECTION field (gusts change
//    heading) and a busier STRENGTH field eased so gusts bite, lulls rest.
//  - VIEW-SPACE THICKENING: blades seen edge-on widen (easeOut^4) so the
//    meadow never dissolves into hairlines.
//  - NORMALS: two per blade, the curve normal rotated ±54° around the
//    blade axis, blended across the width in the fragment; both pulled
//    toward the GROUND normal (dominant far, 35% blade near) — keeps the
//    validated BotW "meadow lights as one surface" grounding.
//  - COLOR: dark rooted base to bright tip, easeIn^4 ramp; per-blade tint
//    and shade hashes; far LOD flattens to a cheap linear ramp.
//
// KEPT CONTRACTS (do not break):
//  - density-LOD clip: instances are SORTED by aParams.y; the curve here
//    must stay identical to GrassSystem::draw()'s prefix cut (both read
//    the same tuning: uGrassLodInfo here, renderTuning there).
//  - the distance fade (uGrass*Color.w) matches draw()'s fadeEnd cull.
//  - uGrassBendInfo player push.

layout(location = 0) in vec2 aBlade;    // x = side [-1,1], y = t [0,1]
layout(location = 2) in vec4 aPosScale; // xyz = terrain point, w = height scale
layout(location = 3) in vec4 aParams;   // x = yaw, y = keep key (sorted),
                                        // z = tint jitter, w = lean
layout(location = 4) in vec4 aGroundNormal; // xyz = terrain normal at root

layout(location = 0) out vec3 vColor;
layout(location = 1) out float vT;
layout(location = 2) out float vXSide;  // 0..1 across the blade width
layout(location = 3) out float vLodOut; // high-detail fade-out [0,1]
layout(location = 4) out vec3 vNormal1;
layout(location = 5) out vec3 vNormal2;
layout(location = 6) out vec3 vWorldPos;

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
mat3 rotAxis(vec3 axis, float angle) {
    float s = sin(angle), c = cos(angle);
    float oc = 1.0 - c;
    return mat3(oc * axis.x * axis.x + c,
                oc * axis.x * axis.y + axis.z * s,
                oc * axis.z * axis.x - axis.y * s,
                oc * axis.x * axis.y - axis.z * s,
                oc * axis.y * axis.y + c,
                oc * axis.y * axis.z + axis.x * s,
                oc * axis.z * axis.x + axis.y * s,
                oc * axis.y * axis.z - axis.x * s,
                oc * axis.z * axis.z + c);
}
mat3 rotX(float angle) {
    float s = sin(angle), c = cos(angle);
    return mat3(1.0, 0.0, 0.0, 0.0, c, s, 0.0, -s, c);
}

void main() {
    // Density-LOD clip — SAME curve as GrassSystem::draw()'s prefix cut.
    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    float thin = smoothstep(uGrassLodInfo.x, uGrassLodInfo.y, dist);
    float density = mix(1.0, uGrassLodInfo.z, thin);
    if (aParams.y * 0.15915494 > density) {
        gl_Position = vec4(2.0, 0.0, 2.0, 1.0); // off-clip, zero raster
        return;
    }

    float t = aBlade.y;
    vT = t;
    vXSide = aBlade.x * 0.5 + 0.5;
    float lodOut = smoothstep(uGrassShapeInfo.z, uGrassShapeInfo.w, dist);
    vLodOut = lodOut;

    // Extra per-blade hashes (stable: seeded by the root position).
    float hashColor1 = hash21(aPosScale.xz * 3.17);
    float hashColor2 = hash21(aPosScale.xz * 7.91);
    float shade = mix(0.65, 1.0, hash21(aPosScale.xz * 5.53));

    // Distance fade: blades sink into the ground (matches draw()'s cull).
    float fade =
        1.0 - smoothstep(uGrassBaseColor.w, uGrassTipColor.w, dist);
    float height = uGrassShapeInfo.x * aPosScale.w * fade;

    // Player push: compute from the root before shaping.
    float push = 0.0;
    vec2 pushDir = vec2(0.0);
    if (uGrassBendInfo.w > 0.0) {
        vec2 away = aPosScale.xz - uGrassBendInfo.xy;
        float d = length(away);
        if (d < uGrassBendInfo.w &&
            abs(aPosScale.y - uGrassBendInfo.z) < 2.0) {
            push = 1.0 - d / uGrassBendInfo.w;
            push *= push;
            pushDir = away / max(d, 0.05);
            height *= 1.0 - push * 0.45; // squash under the step
        }
    }

    // Width: rounded taper near (1 - t^2), linear far; per-blade width
    // jitter; wider blades compensate the far density floor.
    float taper = mix(1.0 - t * t, 1.0 - t, lodOut);
    float halfWidth = uGrassShapeInfo.y * mix(0.7, 1.3, aParams.z) *
                      (1.0 + thin * uGrassLodInfo.w);

    // WIND — the reference's two-noise model on our shared wind clock.
    float dirNoise = vnoise(aPosScale.xz * 0.05 + uWindInfo.x * 0.05);
    float windAngle = 0.34 + (dirNoise * 2.0 - 1.0) * 0.9; // around heading
    vec3 windAxis = vec3(cos(windAngle), 0.0, sin(windAngle));
    float gustNoise = vnoise(aPosScale.xz * 0.25 + uWindInfo.x);
    float windLean = mix(0.25, 1.0, gustNoise);
    windLean = windLean * windLean * 1.25 * t * uWindInfo.y;

    // Per-blade lean + the slow "breathing" wobble.
    float leanAnim =
        (vnoise(vec2(uWindInfo.x * 0.35) + aPosScale.xz * 137.423) * 2.0 -
         1.0) * 0.1;
    float lean = mix(0.15, 0.45, aParams.w) + leanAnim;

    // Blade frame: local x = width, y = up, faces +Z before yaw.
    mat3 grassMat = rotAxis(windAxis, -windLean) *
                    rotAxis(vec3(0.0, 1.0, 0.0), aParams.x);

    // View-space thickening: widen edge-on blades (reference curve).
    vec3 faceNormal = grassMat * vec3(0.0, 0.0, 1.0);
    vec3 viewDirXZ =
        normalize(vec3(uCameraPos.x - aPosScale.x, 0.0,
                       uCameraPos.z - aPosScale.z));
    float viewDot =
        abs(dot(normalize(vec3(faceNormal.x, 0.0, faceNormal.z)), viewDirXZ));
    float thicken = (1.0 - pow(viewDot, 4.0)); // easeOut(1-viewDot, 4)
    thicken *= smoothstep(0.0, 0.2, viewDot);
    halfWidth *= 1.0 + thicken * 0.6;

    // The forward ARC: each vertex rotates around the blade side axis by
    // an angle growing with eased height (easeIn^2 near, linear far).
    float easedT = mix(t * t, t, lodOut);
    float curve = -lean * easedT;
    vec3 local = vec3(aBlade.x * halfWidth * taper, t * height, 0.0);
    local = rotX(curve) * local;
    vec3 world = aPosScale.xyz + grassMat * local;
    world.xz += pushDir * (push * 0.9 * t * height); // player push

    // Rounded normals: the curved face normal rotated ±54° around the
    // blade axis, blended across the width in the fragment; both pulled
    // toward the GROUND normal (dominant far — the BotW grounding).
    vec3 nLocal = rotX(curve) * vec3(0.0, 0.0, 1.0);
    vec3 n1 = grassMat * (rotAxis(vec3(0.0, 1.0, 0.0), 0.94) * nLocal);
    vec3 n2 = grassMat * (rotAxis(vec3(0.0, 1.0, 0.0), -0.94) * nLocal);
    float bladeRatio = (1.0 - lodOut) * 0.35;
    vNormal1 = normalize(mix(aGroundNormal.xyz, n1, bladeRatio));
    vNormal2 = normalize(mix(aGroundNormal.xyz, n2, bladeRatio));

    // COLOR — steep dark-base -> bright-tip ramp (easeIn^4: the base stays
    // dark deep up the blade, the last quarter ignites). The panel colors
    // are the LOW end; per-blade hashes push up to ~1.3x brighter.
    vec3 base = uGrassBaseColor.rgb * mix(1.0, 1.3, hashColor1);
    vec3 tip = uGrassTipColor.rgb * mix(1.0, 1.3, hashColor2);
    float ramp = t * t * t * t;
    vec3 highColor = mix(base, tip, ramp) * shade;
    vec3 lowColor = mix(uGrassBaseColor.rgb, uGrassTipColor.rgb, t);
    vColor = mix(highColor, lowColor, lodOut);

    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
