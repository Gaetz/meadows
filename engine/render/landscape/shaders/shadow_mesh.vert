#version 460 core

// Chantier 6 B2a: scene meshes cast into the sun cascades. Depth-only
// mirror of mesh.vert — position through the per-draw model matrix, then
// the cascade's light matrix. CasterModelUbo rides binding 4 so it never
// collides with ShadowUbo (binding 1, the caster group contract).

layout(location = 0) in vec3 aPos;

layout(std140, binding = 1) uniform ShadowUbo { mat4 uLightViewProj; };
layout(std140, binding = 4) uniform CasterModelUbo { mat4 uCasterModel; };

void main() {
    gl_Position = uLightViewProj * (uCasterModel * vec4(aPos, 1.0));
}
