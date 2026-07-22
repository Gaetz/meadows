#version 460 core
#include "common.glsl"

// GPU-skinned mesh: SkinnedVertex layout + the bone
// palette as an SSBO (skin matrices = model x inverseBind, uploaded per
// character per frame). ModelUbo carries the entity's world transform on
// top — the palette is in model space (anim::skinMatrices).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec4 aJoints;  // palette indices, float-encoded
layout(location = 5) in vec4 aWeights;

layout(std140, binding = 1) uniform ModelUbo {
    mat4 uModel;
    vec4 uTint;      // MaterialForm.tint
    vec4 uMeshInfo;  // x = emissive
};

layout(std430, binding = 2) readonly buffer BonePalette {
    mat4 uBones[];
};

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorldPos;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec3 vColor;

void main() {
    mat4 skin = aWeights.x * uBones[int(aJoints.x)] +
                aWeights.y * uBones[int(aJoints.y)] +
                aWeights.z * uBones[int(aJoints.z)] +
                aWeights.w * uBones[int(aJoints.w)];
    const vec4 world = uModel * skin * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * mat3(skin) * aNormal; // uniform scale assumed
    vWorldPos = world.xyz;
    vUv = aUv;
    vColor = aColor;
    gl_Position = uViewProj * world;
}
