#version 450

#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require

#include "inputStructures.glsl"

layout (location = 0) out vec3 outNormal;
layout (location = 1) out vec2 outUV;
layout (location = 2) out vec3 outWorldPos;

struct Vertex {
    vec3 position;
    float uvX;
    vec3 normal;
    float uvY;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer VertexBuffer{
    Vertex vertices[];
};

layout(push_constant) uniform constants {
    mat4 worldMatrix;
    VertexBuffer vertexBuffer;
} PushConstants;

void main() {
    Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
    
    vec4 worldPos = PushConstants.worldMatrix * vec4(v.position, 1.0);
    outWorldPos = worldPos.xyz;
    outUV = vec2(v.uvX, v.uvY);
    
    // Normal in world space
    mat3 normalMatrix = transpose(inverse(mat3(PushConstants.worldMatrix)));
    outNormal = normalMatrix * v.normal;
    
    gl_Position = sceneData.viewProj * worldPos;
}
