#version 450

#extension GL_GOOGLE_include_directive : require
#include "inputStructures.glsl"

layout (set = 0, binding = 1) uniform sampler2D shadowMap;

layout (location = 0) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

float LinearizeDepth(float depth)
{
    float n = sceneData.shadowParams.x;  // zNear
    float f = sceneData.shadowParams.y;  // zFar
    return (2.0 * n) / (f + n - depth * (f - n));
}

void main()
{
    float depth = texture(shadowMap, inUV).r;
    // Invert for better visualization (closer = brighter)
    outFragColor = vec4(vec3(1.0 - LinearizeDepth(depth)), 1.0);
}
