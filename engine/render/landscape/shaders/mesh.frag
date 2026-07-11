#version 460 core
#include "common.glsl"
#include "sky.glsl"
#include "stylized.glsl"
#include "locallights.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"

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
    // Brick 31 wetness (exterior only — the interior flag gates it).
    albedo *= mix(1.0, 0.75,
                  clamp(uStormInfo.y, 0.0, 1.0) *
                      (1.0 - uCascadeSplits.w));
    vec3 n = normalize(vNormal);
    float ndl = dot(n, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    // B5 interior hemispheric ambient (uCascadeSplits.w = 1 indoors): the
    // dead-flat constant ambient is the #1 "ugly interior" cause — floors
    // catch the bright/cool top light, ceilings the warm dark floor
    // bounce, walls sit between. Exterior stays byte-identical.
    vec3 ambient = uAmbientColor.rgb;
    if (uCascadeSplits.w > 0.5) {
        float up = n.y * 0.5 + 0.5;
        ambient *= mix(vec3(0.50, 0.42, 0.36), vec3(1.35, 1.30, 1.22), up);
    }
    // 33b/c: long-range terrain sun shadow (x) + sky openness (y).
    vec2 tl = terrainLightFactors(vWorldPos);
    // Chantier RC (G6): the ONE technique branch — Classic stays intact.
    vec3 lit = albedo * (giAmbient(vWorldPos, n, ambient * tl.y) +
                         uSunColor.rgb * (diffuse * tl.x) +
                         localLights(vWorldPos, n)) +
               albedo * uMeshInfo.x;
    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
