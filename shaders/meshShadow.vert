#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "inputStructures.glsl"

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec3 outColor;
layout (location = 2) out vec2 outUV;
layout (location = 3) out vec4 outShadowCoord;

struct Vertex {
    vec3 position;
    float uvX;
    vec3 normal;
    float uvY;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer {
    Vertex vertices[];
};

layout(push_constant) uniform constants {
    mat4 renderMatrix;
    VertexBuffer vertexBuffer;
} PushConstants;

// Bias matrix to convert from clip space [-1,1] to texture space [0,1]
const mat4 biasMat = mat4(
    0.5, 0.0, 0.0, 0.0,
    0.0, 0.5, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.5, 0.5, 0.0, 1.0
);

void main()
{
    Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];

    vec4 position = vec4(v.position, 1.0f);

    gl_Position = sceneData.viewProj * PushConstants.renderMatrix * position;

    outNormal = (PushConstants.renderMatrix * vec4(v.normal, 0.f)).xyz;
    outColor = v.color.xyz * materialData.colorFactors.xyz;
    outUV.x = v.uvX;
    outUV.y = v.uvY;

    // Calculate shadow coordinate in light space with bias
    outShadowCoord = biasMat * sceneData.lightSpaceMatrix * PushConstants.renderMatrix * position;
}
