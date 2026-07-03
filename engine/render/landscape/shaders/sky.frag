#version 460 core
#include "common.glsl"
#include "sky.glsl"

in vec3 vRay;
out vec4 fragColor;

void main() {
    fragColor = vec4(skyWithSun(normalize(vRay)), 1.0);
}
