#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "stylized.glsl"
#include "locallights.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"

// Shading twin of mesh.frag (albedo texture x tint x vertex color, shared
// BotW ramp, shared fog) — KEEP IN SYNC with mesh.frag; only the vertex
// stage differs (bone palette).

layout(binding = 0) uniform sampler2D uAlbedo;

layout(std140, binding = 1) uniform ModelUbo {
    mat4 uModel;
    vec4 uTint;
    vec4 uMeshInfo; // x = emissive
};

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec3 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 albedo = texture(uAlbedo, vUv).rgb * uTint.rgb * vColor;
    // Wetness — KEEP IN SYNC with mesh.frag.
    albedo *= mix(1.0, 0.75,
                  clamp(uStormInfo.y, 0.0, 1.0) *
                      (1.0 - uCascadeSplits.w));
    vec3 n = normalize(vNormal);
    float ndl = dot(n, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    // Interior hemispheric ambient — KEEP IN SYNC with mesh.frag.
    vec3 ambient = uAmbientColor.rgb;
    if (uCascadeSplits.w > 0.5) {
        float up = n.y * 0.5 + 0.5;
        ambient *= mix(vec3(0.50, 0.42, 0.36), vec3(1.35, 1.30, 1.22), up);
    }
    // Stylized specular band — KEEP IN SYNC with mesh.frag.
    vec3 viewDir = normalize(uCameraPos.xyz - vWorldPos);
    vec3 halfDir = normalize(uSunDirection.xyz + viewDir);
    float spec = stylizedSpec(pow(max(dot(n, halfDir), 0.0),
                                  uStylizedSpec.z),
                              uStylizedSpec.y) *
                 uStylizedSpec.x * diffuse * uAmbientColor.w;
    // Terrain light factors — KEEP IN SYNC with mesh.frag.
    vec2 tl = terrainLightFactors(vWorldPos);
    // The GI technique branch — KEEP IN SYNC with mesh.frag.
    vec3 lit = albedo * (giAmbient(vWorldPos, n, ambient * tl.y) +
                         uSunColor.rgb * ((diffuse + spec) * tl.x) +
                         localLights(vWorldPos, n)) +
               albedo * uMeshInfo.x;
    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
