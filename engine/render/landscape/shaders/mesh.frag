#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "stylized.glsl"
#include "locallights.glsl"

// Textured stylized mesh (H8): flat albedo texture x material tint x
// vertex color, lit by the shared BotW ramp, faded by the shared fog.
// CSM/cloud shadow receive joins with the mesh vertical (binding 1 UBO is
// the ModelUbo here; the shadow map keeps unit 1 in the full path).

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
