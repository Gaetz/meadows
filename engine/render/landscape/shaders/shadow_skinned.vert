#version 460 core

// Skinned characters cast into the sun cascades. Depth-only
// mirror of skinned.vert — the bone palette (SSBO binding 2, same buffer as
// the lit pass) then the per-NPC model matrix (CasterModelUbo binding 4;
// ShadowUbo keeps binding 1 per the caster group contract).

layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJoints;  // palette indices, float-encoded
layout(location = 5) in vec4 aWeights;

layout(std140, binding = 1) uniform ShadowUbo { mat4 uLightViewProj; };
layout(std140, binding = 4) uniform CasterModelUbo { mat4 uCasterModel; };

layout(std430, binding = 2) readonly buffer BonePalette {
    mat4 uBones[];
};

void main() {
    mat4 skin = aWeights.x * uBones[int(aJoints.x)] +
                aWeights.y * uBones[int(aJoints.y)] +
                aWeights.z * uBones[int(aJoints.z)] +
                aWeights.w * uBones[int(aJoints.w)];
    gl_Position = uLightViewProj * (uCasterModel * skin * vec4(aPos, 1.0));
}
