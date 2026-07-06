#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "stylized.glsl"
#include "locallights.glsl"

// Shading twin of mesh.frag (albedo texture x tint x vertex color, shared
// BotW ramp, shared fog) — KEEP IN SYNC with mesh.frag; only the vertex
// stage differs (bone palette).

layout(binding = 0) uniform sampler2D uAlbedo;

layout(std140, binding = 1) uniform ModelUbo {
    mat4 uModel;
    vec4 uTint;
    vec4 uMeshInfo; // x = emissive
};

in vec3 vNormal;
in vec3 vWorldPos;
in vec2 vUv;
in vec3 vColor;
out vec4 fragColor;

void main() {
    vec3 albedo = texture(uAlbedo, vUv).rgb * uTint.rgb * vColor;
    vec3 n = normalize(vNormal);
    float ndl = dot(n, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    vec3 lit = albedo * (uAmbientColor.rgb + uSunColor.rgb * diffuse +
                         localLights(vWorldPos, n)) +
               albedo * uMeshInfo.x;
    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
