#version 460 core
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec3 vWorldPos;

void main() {
    vNormal = aNormal;
    vColor = aColor;
    vWorldPos = aPos;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
