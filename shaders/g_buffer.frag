#version 450

#extension GL_GOOGLE_include_directive : require

#include "inputStructures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec2 inUV;
layout (location = 2) in vec3 inWorldPos;

layout (location = 0) out vec4 outPosition;
layout (location = 1) out vec4 outNormal;
layout (location = 2) out vec4 outAlbedo;

void main() {
    outPosition = vec4(inWorldPos, 1.0);
    outNormal = vec4(normalize(inNormal), 1.0);
    outAlbedo = texture(colorTex, inUV);
}
