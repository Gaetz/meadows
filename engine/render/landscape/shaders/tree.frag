#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vTint;

layout(location = 0) out vec4 fragColor;

void main() {
    // Per-instance hue roll: some trees lean yellow-green, some deep green.
    vec3 albedo = vColor * mix(vec3(0.85, 1.0, 0.75), vec3(1.1, 1.0, 1.15),
                               vTint);

    albedo *= cascadeDebugTint(vWorldPos);
    vec3 n = normalize(vNormal);
    // Classic mode: wrap diffuse (soft-GI feel). Stylized mode: the shared
    // BotW step ramp — flat lit/shade plateaus over the faceted masses.
    float ndl = dot(n, uSunDirection.xyz);
    float wrap = clamp((ndl + 0.4) / 1.4, 0.0, 1.0);
    float diffuse = stylizedDiffuse(ndl, wrap);
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) *
                   cloudShadowFactor(vWorldPos);
    vec3 lit =
        albedo * (uAmbientColor.rgb + uSunColor.rgb * (diffuse * shadow));
    // Stepped rim against the sky — canopies pop off the
    // background (moved here from the removed leaf-card pass).
    lit += albedo * stylizedRim(n, vWorldPos) * uSunColor.rgb * shadow;

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
