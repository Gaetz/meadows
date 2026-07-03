#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "clouds.glsl"

in vec3 vRay;
out vec4 fragColor;

void main() {
    vec3 dir = normalize(vRay);
    fragColor = vec4(applyClouds(skyWithSun(dir), dir), 1.0);
}
