#version 460 core
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aShade;
layout(location = 2) in vec3 aOffset;
layout(location = 3) in vec3 aTint;

out vec3 vColor;

void main() {
    vColor = aShade * aTint;
    gl_Position = uViewProj * vec4(aPos + aOffset, 1.0);
}
