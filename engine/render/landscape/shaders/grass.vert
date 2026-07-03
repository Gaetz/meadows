#version 460 core
#include "common.glsl"

layout(location = 0) in vec2 aBlade;    // x = side [-1,1] (taper baked), y = t
layout(location = 2) in vec4 aPosScale; // xyz = terrain point, w = height scale
layout(location = 3) in vec4 aParams;   // x = yaw, y = flutter phase,
                                        // z = tint jitter, w = lean

out float vT;
out float vSide;
out float vTint;
out vec3 vNormal;
out vec3 vWorldPos;

const float kBladeHeight = 0.62;    // meters at scale 1
const float kBladeHalfWidth = 0.045;

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

    // Static lean (each blade settles differently) + layered wind: a slow
    // traveling gust front plus a fast per-blade flutter. All of it bends
    // quadratically along the blade so the root stays planted.
    float gust = 0.5 + 0.5 * sin(uTime.x * 1.4 +
                                 (aPosScale.x + aPosScale.z * 0.7) * 0.07);
    float flutter = sin(uTime.x * 5.1 + aParams.y * 6.2831853);
    vec3 windDir = normalize(vec3(1.0, 0.0, 0.35));
    float bendT = t * t;
    vec3 bend = faceDir * (aParams.w * 0.30 * bendT) +
                windDir * ((0.22 * gust + 0.06 * flutter) * bendT);

    vec3 world = aPosScale.xyz + sideDir * (aBlade.x * kBladeHalfWidth) +
                 vec3(0.0, height * t, 0.0) + bend * height;

    // Rounded-ribbon normal: mostly up, tilted by the side coordinate so the
    // two halves of a blade catch light differently (fake cylinder shading).
    vNormal = normalize(vec3(0.0, 1.0, 0.0) + sideDir * (aBlade.x * 0.55) -
                        (bend * 0.8));

    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
