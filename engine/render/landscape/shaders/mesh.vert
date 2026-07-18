#version 460 core
#include "common.glsl"

// Textured stylized mesh (H8 contract proof): MeshVertex layout + a
// per-draw ModelUbo. The full mesh path (instancing, residency cache,
// skinning palettes) is the "socle 3D gameplay" vertical.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;

layout(std140, binding = 1) uniform ModelUbo {
    mat4 uModel;
    vec4 uTint;      // MaterialForm.tint
    vec4 uMeshInfo;  // x = emissive
};

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec3 vColor;

void main() {
    const vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal; // uniform scale assumed for the proof
    vWorldPos = world.xyz;
    vUv = aUv;
    vColor = aColor;
    gl_Position = uViewProj * world;
}
