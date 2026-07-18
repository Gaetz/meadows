#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "clouds.glsl"

layout(location = 0) in vec3 vRay;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 dir = normalize(vRay);
    fragColor = vec4(applyClouds(skyWithSun(dir), dir), 1.0);
}
